/**
 *  DTVRecorder -- base class for DVBRecorder and HDTVRecorder
 *  Copyright 2003-2004 by Brandon Beattie, Doug Larrick, 
 *    Jason Hoos, and Daniel Thor Kristjansson
 *  Distributed as part of MythTV under GPL v2 and later.
 */

using namespace std;

#include "RingBuffer.h"
#include "programinfo.h"
#include "mpegtables.h"
#include "dtvrecorder.h"
#include "tv_rec.h"

#define LOC QString("DTVRec(%1): ").arg(tvrec->GetCaptureCardNum())
#define LOC_ERR QString("DTVRec(%1) Error: ").arg(tvrec->GetCaptureCardNum())

const uint DTVRecorder::kMaxKeyFrameDistance = 32;

/** \class DTVRecorder
 *  \brief This is a specialization of RecorderBase used to
 *         handle DVB and ATSC streams.
 *
 *  This class is an abstract class. If you are using a
 *  pcHDTV card with the bttv drivers, ATSC streams are
 *  handled by the HDTVRecorder. If you are using DVB
 *  drivers DVBRecorder is used. If you are using firewire
 *  cable box input the FirewireRecorder is used.
 *
 *  \sa DVBRecorder, HDTVRecorder, FirewireRecorder, DBox2Recorder
 */

DTVRecorder::DTVRecorder(TVRec *rec, const char *name) : 
    RecorderBase(rec, name),
    // file handle for stream
    _stream_fd(-1),
    // used for scanning pes headers for keyframes
    _keyframe_seen(false),          _position_within_gop_header(0),
    _first_keyframe(0),             _last_keyframe_seen(0),
    _last_gop_seen(0),              _last_seq_seen(0),
    // settings
    _request_recording(false),
    _wait_for_keyframe_option(true),
    _wait_for_keyframe(true),
    // state
    _recording(false),
    _streamid_not_in_first_ts_packet(false),
    _error(false),
    // TS packet buffer
    _buffer(0),                     _buffer_size(0),
    // statistics
    _frames_seen_count(0),          _frames_written_count(0)
{
}

DTVRecorder::~DTVRecorder()
{
    if (_streamid_not_in_first_ts_packet)
    {
        VERBOSE(VB_IMPORTANT, LOC +
                "Saw keyframe stream_id which was not in first TS Packet.");
    }
}

/** \fn DTVRecorder::SetOption(const QString&,int)
 *  \brief handles the "wait_for_seqstart" and "pkt_buf_size" options.
 */
void DTVRecorder::SetOption(const QString &name, int value)
{
    if (name == "wait_for_seqstart")
        _wait_for_keyframe = _wait_for_keyframe_option = (value == 1);
    else if (name == "pkt_buf_size")
    {
        if (_request_recording)
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR +
                    "Attempt made to resize packet buffer while recording.");
            return;
        }
        int newsize = max(value - (value % TSPacket::SIZE), TSPacket::SIZE*50);
        unsigned char* newbuf = new unsigned char[newsize];
        if (newbuf) {
            memcpy(newbuf, _buffer, min(_buffer_size, newsize));
            memset(newbuf+_buffer_size, 0xFF, max(newsize-_buffer_size, 0));
            _buffer = newbuf;
            _buffer_size = newsize;
        }
        else
            VERBOSE(VB_IMPORTANT, LOC_ERR + 
                    "Could not allocate new packet buffer.");
    }
}

/** \fn DTVRecorder::FinishRecording(void)
 *  \brief Flushes the ringbuffer, and if this is not a live LiveTV
 *         recording saves the position map and filesize.
 */
void DTVRecorder::FinishRecording(void)
{
    if (ringBuffer)
        ringBuffer->WriterFlush();

    if (curRecording)
    {
        if (ringBuffer)
            curRecording->SetFilesize(ringBuffer->GetRealFileSize());
        SavePositionMap(true);
    }
    _position_map_lock.lock();
    _position_map.clear();
    _position_map_lock.unlock();
}

// documented in recorderbase.h
long long DTVRecorder::GetKeyframePosition(long long desired)
{
    QMutexLocker locker(&_position_map_lock);
    long long ret = -1;

    if (_position_map.find(desired) != _position_map.end())
        ret = _position_map[desired];

    return ret;
}

// documented in recorderbase.h
void DTVRecorder::Reset(void)
{
    QMutexLocker locker(&_position_map_lock);

    _keyframe_seen              = false;
    _position_within_gop_header = 0;
    _first_keyframe             = 0;
    _last_keyframe_seen         = 0;
    _last_gop_seen              = 0;
    _last_seq_seen              = 0;
    //_recording
    //_streamid_not_in_first_ts_packet
    _error                      = false;
    //_buffer
    //_buffer_size
    _frames_seen_count          = 0;
    _frames_written_count       = 0;
    _position_map.clear();
    _position_map_delta.clear();
}

/** \fn DTVRecorder::FindKeyframes(const TSPacket* tspacket)
 *  \brief Locates the keyframes and saves them to the position map.
 *
 *   This searches for three magic integers in the stream.
 *   The picture start code 0x00000100, the GOP code 0x000001B8,
 *   and the sequence start code 0x000001B3. The GOP code is
 *   prefered, but is only required of MPEG1 streams, the
 *   sequence start code is a decent fallback for MPEG2 
 *   streams, and if all else fails we just look for the picture
 *   start codes and call every 16th frame a keyframe.
 *
 *   NOTE: This does not only find keyframes but also tracks the
 *         total frames as well. At least a couple times seeking
 *         has been broken by short-circuiting the search once
 *         a keyframe stream id has been found. This may work on
 *         some channels, but will break on others so algorithmic
 *         optimizations should be done with great care.
 *
 * \startcode
 *   PES header format
 *   byte 0  byte 1  byte 2  byte 3      [byte 4     byte 5]
 *   0x00    0x00    0x01    PESStreamID  PES packet length
 * \endcode
 *
 */
void DTVRecorder::FindKeyframes(const TSPacket* tspacket)
{
    if (!tspacket->HasPayload())
        return; // no payload to scan

    // if packet contains start of PES packet, start
    // looking for first byte of MPEG start code (3 bytes 0 0 1)
    // otherwise, pick up search where we left off.
    const bool payloadStart = tspacket->PayloadStart();
    _position_within_gop_header = (payloadStart) ?
        0 : _position_within_gop_header;

    // Just make these local for efficiency reasons (gcc not so smart..)
    const uint maxKFD            = kMaxKeyFrameDistance;
    const long long frameSeenNum = _frames_seen_count;
    const unsigned char *buffer  = tspacket->data();

    bool hasKeyFrame = false;

    // Scan for PES header codes; specifically picture_start
    // and group_start (of_pictures).  These should be within
    // this first TS packet of the PES packet.
    //   00 00 01 00: picture_start_code
    //   00 00 01 B8: group_start_code
    //   00 00 01 B3: seq_start_code
    //   (there are others that we don't care about)
    uint i = tspacket->AFCOffset();
    for (; (i+1) < TSPacket::SIZE; i++)
    {
        const unsigned char k = buffer[i];
        if (0 == _position_within_gop_header)
            _position_within_gop_header = (k == 0x00) ? 1 : 0;
        else if (1 == _position_within_gop_header)
            _position_within_gop_header = (k == 0x00) ? 2 : 0;
        else // if (2 == _position_within_gop_header)
        {
            if (0x01 != k)
            {
                _position_within_gop_header = (k == 0x00) ? 2 : 0;
                continue;
            }

            // At this point we have seen the start code 0 0 1
            // the next byte will be the PES packet stream id.

            const int stream_id = buffer[i+1];
            if (PESStreamID::PictureStartCode == stream_id)
            {
                _frames_written_count += (_first_keyframe > 0) ? 1 : 0;
                _frames_seen_count++;

                // If we have seen kMaxKeyFrameDistance frames since the
                // last GOP or SEQ stream_id, then pretend this picture
                // is a keyframe. We may get artifacts but at least
                // we will be able to skip frames.
                hasKeyFrame |= (0 == (_frames_seen_count & 0xf)) &&
                    (_last_gop_seen + maxKFD < frameSeenNum) &&
                    (_last_seq_seen + maxKFD < frameSeenNum);
            }
            else if (PESStreamID::GOPStartCode == stream_id)
            {
                _last_gop_seen  = frameSeenNum;
                hasKeyFrame    |= true;
            }
            else if (PESStreamID::SequenceStartCode == stream_id)
            {
                _last_seq_seen  = frameSeenNum;
                hasKeyFrame    |= _last_gop_seen + maxKFD < frameSeenNum;
            }
            _position_within_gop_header = 0;
        }
    }

    if (hasKeyFrame)
    {
        _last_keyframe_seen = frameSeenNum;
        HandleKeyframe();

        if (!payloadStart && !_streamid_not_in_first_ts_packet)
        {
            VERBOSE(VB_IMPORTANT, LOC +
                    "Keyframe stream_id not in first TS Packet.\n"
                    "This is not an error, but MythTV may not always handle\n"
                    "this gracefully. If you would like it to, please inform\n"
                    "the mythtv developer mailing list of this message.\n");
            // If this happens in practice, it means we need to buffer
            // all TS packets after the first payload start until
            // we determine the PES stream ID; and then only write
            // data to the ringbuffer after we determine the PES stream ID
            // and call HandleKeyframe() if needed.
            _streamid_not_in_first_ts_packet = true;
        }
    }
}

// documented in recorderbase.h
void DTVRecorder::SetNextRecording(const ProgramInfo *progInf, RingBuffer *rb)
{
    VERBOSE(VB_RECORD, LOC + "SetNextRecord("<<progInf<<", "<<rb<<")");
    // First we do some of the time consuming stuff we can do now
    SavePositionMap(true);
    if (ringBuffer)
        ringBuffer->WriterFlush();

    // Then we set the next info
    nextRingBufferLock.lock();

    nextRecording = NULL;
    if (progInf)
        nextRecording = new ProgramInfo(*progInf);

    nextRingBuffer = rb;
    nextRingBufferLock.unlock();
}

void DTVRecorder::ResetForNewFile(void)
{
    VERBOSE(VB_RECORD, LOC + "ResetForNewFile(void)");
    Reset();
}

/** \fn DTVRecorder::HandleKeyframe(void)
 *  \brief This save the current frame to the position maps
 *         and handles ringbuffer switching.
 */
void DTVRecorder::HandleKeyframe(void)
{
    unsigned long long frameNum = _frames_written_count ?
        _frames_written_count - 1 : 0;

    if (!_keyframe_seen && _frames_seen_count)
    {
        if (_first_keyframe > 0)
            _keyframe_seen = true;
        else
            _first_keyframe = (_frames_written_count) ? frameNum : 1;
    }

    // Add key frame to position map
    bool save_map = false;
    _position_map_lock.lock();
    if (!_position_map.contains(frameNum))
    {
        long long startpos = ringBuffer->GetWritePosition();
        _position_map_delta[frameNum] = startpos;
        _position_map[frameNum]       = startpos;
        save_map = true;
    }
    _position_map_lock.unlock();
    // Save the position map delta, but don't force a save.
    if (save_map)
        SavePositionMap(false);

    // Perform ringbuffer switch if needed.
    CheckForRingBufferSwitch();
}

/** \fn DTVRecorder::SavePositionMap(bool)
 *  \brief This saves the postition map delta to the database if force
 *         is true or there are 30 frames in the map or there are five
 *         frames in the map with less than 30 frames in the non-delta
 *         position map.
 *  \param force If true this forces a DB sync.
 */
void DTVRecorder::SavePositionMap(bool force)
{
    QMutexLocker locker(&_position_map_lock);

    // save on every 5th key frame if in the first few frames of a recording
    force |= (_position_map.size() < 30) && (_position_map.size()%5 == 1);
    // save every 30th key frame later on
    force |= _position_map_delta.size() >= 30;

    if (curRecording && force && _position_map_delta.size())
    {
        curRecording->SetPositionMapDelta(_position_map_delta, 
                                          MARK_GOP_BYFRAME);
        _position_map_delta.clear();

        if (ringBuffer)
            curRecording->SetFilesize(ringBuffer->GetWritePosition());
    }
}
