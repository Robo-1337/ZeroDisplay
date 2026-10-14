#include "encode/Recorder.h"
#include "encode/Mp4File.h"
#include "core/Log.h"
#include "encode/H264.h"

#include <mferror.h>

#include <algorithm>
#include <cmath>

namespace cc {

Recorder::~Recorder() { Stop(); }

std::wstring Recorder::Error() const
{
    std::lock_guard lock(m_errorMutex);
    return m_error;
}

void Recorder::Fail(const std::wstring& what, HRESULT hr)
{
    LOGE(L"Recorder: %s failed: %s", what.c_str(), HrToString(hr).c_str());
    std::lock_guard lock(m_errorMutex);
    m_error = what + L" failed: " + HrToString(hr);
}

std::wstring Recorder::Path() const
{
    std::lock_guard lock(m_errorMutex);
    return m_path;
}

std::wstring Recorder::PartPath(int part) const
{
    if (part <= 1) return m_basePath;
    std::wstring base = m_basePath;
    std::wstring ext = L".mp4";
    const size_t dot = base.find_last_of(L'.');
    const size_t slash = base.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) {
        ext = base.substr(dot);
        base.resize(dot);
    }
    return base + L"_part" + std::to_wstring(part) + ext;
}

bool Recorder::Start(const std::wstring& path, VideoEncoder& encoder, const RecordOptions& options)
{
    Stop();
    m_basePath = path;
    {
        std::lock_guard lock(m_errorMutex);
        m_path = path;
        m_error.clear();
    }
    m_encoder = &encoder;
    m_cfg = encoder.Config();
    m_splitSeconds = options.splitSeconds;
    m_part = 1;
    m_headerParts = 0;
    m_doneSeconds = 0;
    m_stop = false;
    m_packets.clear();
    m_kfEncoder = &encoder;
    m_kfRequested = false;
    m_seconds = 0;
    m_bytes = 0;
    m_writing = false;
    m_tracks.clear();
    for (const auto& src : options.tracks) {
        if (!src) continue;
        bool dup = false;
        for (const auto& t : m_tracks) dup |= t.source == src;
        if (dup) continue;
        Track t;
        t.source = src;
        t.tap = std::make_shared<AudioTap>();
        src->AddTap(t.tap);
        m_tracks.push_back(std::move(t));
    }
    m_sinkId = encoder.AddSink([this](const PacketPtr& p) {
        {
            std::lock_guard lock(m_mutex);
            m_packets.push_back(p);
        }
        m_cv.notify_one();
    });
    encoder.RequestKeyframe();
    m_active = true;
    m_thread = std::thread([this] { ThreadMain(); });
    LOGI(L"Recording to %s (%zu audio track(s), split %d s)", path.c_str(), m_tracks.size(), m_splitSeconds);
    return true;
}

void Recorder::SetPaused(bool paused)
{
    if (m_pauseWanted.exchange(paused) == paused) return;
    LOGI(L"Recording %s", paused ? L"paused" : L"resumed");
    if (!paused) {   // continue as soon as possible: a keyframe now instead of at the end of the GOP
        std::lock_guard lock(m_mutex);
        if (m_kfEncoder) m_kfEncoder->RequestKeyframe();
    }
    m_cv.notify_one();
}

bool Recorder::Dropped(int64_t time) const
{
    for (const auto& [from, to] : m_pauses)
        if (time >= from && (to < 0 || time < to)) return true;
    return false;
}

int64_t Recorder::Shift(int64_t time) const
{
    int64_t shift = 0;
    for (const auto& [from, to] : m_pauses)
        if (to >= 0 && time >= to) shift += to - from;
    return shift;
}

void Recorder::RequestStop()
{
    if (m_encoder) {
        m_encoder->RemoveSink(m_sinkId);
        m_encoder = nullptr;
    }
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
        m_kfEncoder = nullptr;
    }
    m_cv.notify_one();
}

void Recorder::Stop()
{
    if (!m_active.exchange(false) && !m_thread.joinable()) return;
    RequestStop();
    if (m_thread.joinable()) m_thread.join();
    for (auto& t : m_tracks)
        if (t.source && t.tap) t.source->RemoveTap(t.tap.get());
    m_tracks.clear();
}

bool Recorder::OpenSink(const EncodedPacket& first)
{
    std::vector<uint8_t> ps = m_cfg.hevc ? h264::ExtractParameterSetsHevc(first.data.data(), first.data.size())
                                         : h264::ExtractParameterSets(first.data.data(), first.data.size());
    m_filePs = ps;
    const std::wstring path = Path();

    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 2);
    // Both go into fragmented MP4 (playable after a crash): H.264 through Windows' fragmented MP4 sink,
    // HEVC (which that sink rejects) through our own writer with the same interface.
    a->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_FMPEG4);
    a->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    HRESULT hr = m_cfg.hevc ? CreateFragmentedMp4Writer(path, &m_writer) : MFCreateSinkWriterFromURL(path.c_str(), nullptr, a.Get(), &m_writer);
    if (FAILED(hr)) {
        Fail(L"Creating the file", hr);
        return false;
    }

    ComPtr<IMFMediaType> v;
    MFCreateMediaType(&v);
    v->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    v->SetGUID(MF_MT_SUBTYPE, m_cfg.hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    MFSetAttributeSize(v.Get(), MF_MT_FRAME_SIZE, m_cfg.width, m_cfg.height);
    MFSetAttributeRatio(v.Get(), MF_MT_FRAME_RATE, m_cfg.fpsNum, m_cfg.fpsDen);
    MFSetAttributeRatio(v.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    v->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    v->SetUINT32(MF_MT_AVG_BITRATE, m_cfg.bitrateKbps * 1000);
    // BT.709 (or HDR10: BT.2020, PQ) limited range, also in the container (not every encoder writes it into the stream).
    v->SetUINT32(MF_MT_VIDEO_PRIMARIES, m_cfg.hdr ? MFVideoPrimaries_BT2020 : MFVideoPrimaries_BT709);
    v->SetUINT32(MF_MT_TRANSFER_FUNCTION, m_cfg.hdr ? MFVideoTransFunc_2084 : MFVideoTransFunc_709);
    v->SetUINT32(MF_MT_YUV_MATRIX, m_cfg.hdr ? MFVideoTransferMatrix_BT2020_10 : MFVideoTransferMatrix_BT709);
    v->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    if (!ps.empty()) v->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, ps.data(), UINT32(ps.size()));
    hr = m_writer->AddStream(v.Get(), &m_videoStream);
    if (SUCCEEDED(hr)) hr = m_writer->SetInputMediaType(m_videoStream, v.Get(), nullptr);
    if (FAILED(hr)) {
        Fail(L"Adding the video stream", hr);
        m_writer.Reset();
        return false;
    }

    int audioStreams = 0;
    for (auto& t : m_tracks) {
        t.has = false;
        t.next = -1;
        for (const auto& c : t.pending)
            if (c.rate) t.rate = c.rate;
        for (const auto& c : t.recent)
            if (c.rate) t.rate = c.rate;
        if (t.rate != 44100 && t.rate != 48000) {
            LOGW(L"Recorder: audio at %u Hz can't be stored as AAC; skipping that track", t.rate);
            continue;
        }
        ComPtr<IMFMediaType> aac, pcm;
        MFCreateMediaType(&aac);
        aac->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        aac->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        aac->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        aac->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, t.rate);
        aac->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        aac->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);   // 192 kbps
        MFCreateMediaType(&pcm);
        pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, t.rate);
        pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, t.rate * 4);
        hr = m_writer->AddStream(aac.Get(), &t.stream);
        if (SUCCEEDED(hr)) hr = m_writer->SetInputMediaType(t.stream, pcm.Get(), nullptr);
        if (SUCCEEDED(hr)) {
            t.has = true;
            ++audioStreams;
        } else {
            LOGW(L"Recorder: audio stream unavailable: %s", HrToString(hr).c_str());
        }
    }

    hr = m_writer->BeginWriting();
    if (FAILED(hr)) {
        Fail(L"Starting the file", hr);
        m_writer.Reset();
        return false;
    }
    m_t0 = first.time;
    m_videoEnd = 0;
    std::erase_if(m_pauses, [](const auto& p) { return p.second >= 0; });   // an open pause stays open
    m_writing = true;
    LOGI(L"Recorder: writing %s (%s, %ux%u, %d audio track(s))", path.c_str(), m_cfg.hevc ? L"HEVC" : L"H.264", m_cfg.width, m_cfg.height,
         audioStreams);
    return true;
}

void Recorder::FinishFile()
{
    FlushVideo();
    if (!m_writer) return;
    // Audio that stopped early (device unplugged, standby) is padded with silence so every track
    // spans the whole video.
    for (auto& tr : m_tracks) {
        if (!tr.has) continue;
        const int64_t from = std::max<int64_t>(tr.next, 0);
        if (m_videoEnd - from > 200000) {
            int64_t t = from;
            while (t < m_videoEnd) {
                const uint32_t frames = uint32_t(std::min<int64_t>(tr.rate / 10, (m_videoEnd - t) * tr.rate / 10000000 + 1));
                WritePcm(tr, nullptr, frames, t);
                t += int64_t(frames) * 10000000 / tr.rate;
            }
            tr.next = t;
        }
    }
    HRESULT hr = m_writer->Finalize();
    if (FAILED(hr)) Fail(L"Finalizing the file", hr);
    m_writer.Reset();
    m_doneSeconds = m_seconds.load();
    LOGI(L"Recording finished: %s (%.1f s total, %.1f MB total)", Path().c_str(), m_seconds.load(), m_bytes.load() / 1048576.0);
}

// Casts and network sources have a variable frame rate, and the MP4 sink builds the timeline from sample
// durations: each frame lasts until the next one (the nominal duration only when that isn't known).
void Recorder::WriteVideo(const PacketPtr& p)
{
    if (m_heldVideo) {
        const int64_t d = (p->time - Shift(p->time)) - (m_heldVideo->time - Shift(m_heldVideo->time));
        WriteVideoSample(*m_heldVideo, d > 0 && d < 10000000 ? d : m_heldVideo->duration);
    }
    m_heldVideo = p;
}

void Recorder::FlushVideo(int64_t nextTime)
{
    if (m_heldVideo) {
        const int64_t d = nextTime - m_heldVideo->time;
        WriteVideoSample(*m_heldVideo, nextTime >= 0 && d > 0 && d < 10000000 ? d : m_heldVideo->duration);
    }
    m_heldVideo.reset();
}

void Recorder::WriteVideoSample(const EncodedPacket& p, int64_t duration)
{
    if (!m_writer) return;
    ComPtr<IMFMediaBuffer> buf;
    BYTE* dst = nullptr;
    if (FAILED(MFCreateMemoryBuffer(DWORD(p.data.size()), &buf)) || FAILED(buf->Lock(&dst, nullptr, nullptr))) return;
    memcpy(dst, p.data.data(), p.data.size());
    buf->Unlock();
    buf->SetCurrentLength(DWORD(p.data.size()));
    ComPtr<IMFSample> s;
    MFCreateSample(&s);
    s->AddBuffer(buf.Get());
    const int64_t t = std::max<int64_t>(0, p.time - Shift(p.time) - m_t0);
    s->SetSampleTime(t);
    s->SetSampleDuration(duration);
    s->SetUINT32(MFSampleExtension_CleanPoint, p.keyframe ? TRUE : FALSE);
    HRESULT hr = m_writer->WriteSample(m_videoStream, s.Get());
    if (FAILED(hr)) {
        Fail(L"Writing video", hr);
        return;
    }
    m_bytes += p.data.size();
    m_videoEnd = t + duration;
    m_seconds = m_doneSeconds + double(t + duration) / 1e7;
}

void Recorder::WritePcm(Track& tr, const float* stereo, uint32_t frames, int64_t time)
{
    if (frames == 0) return;
    const DWORD bytes = frames * 4;
    ComPtr<IMFMediaBuffer> buf;
    BYTE* dst = nullptr;
    if (FAILED(MFCreateMemoryBuffer(bytes, &buf)) || FAILED(buf->Lock(&dst, nullptr, nullptr))) return;
    int16_t* out = reinterpret_cast<int16_t*>(dst);
    for (uint32_t i = 0; i < frames * 2; ++i) {
        float v = stereo ? std::clamp(stereo[i], -1.0f, 1.0f) : 0.0f;
        out[i] = int16_t(std::lround(v * 32767.0f));
    }
    buf->Unlock();
    buf->SetCurrentLength(bytes);
    ComPtr<IMFSample> s;
    MFCreateSample(&s);
    s->AddBuffer(buf.Get());
    s->SetSampleTime(time);
    s->SetSampleDuration(int64_t(frames) * 10000000 / tr.rate);
    HRESULT hr = m_writer->WriteSample(tr.stream, s.Get());
    if (FAILED(hr)) Fail(L"Writing audio", hr);
}

void Recorder::WriteAudio(Track& tr, const AudioChunk& c)
{
    if (!tr.has || c.rate != tr.rate || c.stereo.empty()) return;
    if (Dropped(c.time)) return;
    uint32_t frames = uint32_t(c.stereo.size() / 2);
    const float* data = c.stereo.data();
    int64_t t = c.time - Shift(c.time) - m_t0;
    const int64_t dur = int64_t(frames) * 10000000 / tr.rate;
    if (t + dur <= 0) return;   // entirely before the first video frame
    if (tr.next < 0) {
        // Trim the part before the file start.
        if (t < 0) {
            uint32_t skip = uint32_t(std::min<int64_t>(frames, (-t) * tr.rate / 10000000));
            data += size_t(skip) * 2;
            frames -= skip;
            t = 0;
        }
        tr.next = 0;
        if (t > 0) {
            // The sound starts a little after the picture: the MP4 track would still start at 0 (the sound then plays
            // early by up to a chunk), so the lead-in is written as silence.
            uint32_t lead = uint32_t(t * tr.rate / 10000000);
            while (lead > 0) {
                const uint32_t n = std::min(lead, tr.rate / 10);
                WritePcm(tr, nullptr, n, tr.next);
                tr.next += int64_t(n) * 10000000 / tr.rate;
                lead -= n;
            }
            LOGI(L"Recorder: sound starts %.1f ms after the picture (silence before it)", t / 10000.0);
        }
    }
    // Keep the audio timeline continuous but locked to the capture clock: small differences are
    // ignored, larger ones (drift, dropouts) are fixed with a bit of silence or by skipping samples.
    const int64_t drift = t - tr.next;
    const int64_t tolerance = 200000;   // 20 ms
    if (drift > tolerance) {
        uint32_t gap = uint32_t(drift * tr.rate / 10000000);
        WritePcm(tr, nullptr, gap, tr.next);
        tr.next += int64_t(gap) * 10000000 / tr.rate;
    } else if (drift < -tolerance) {
        uint32_t skip = uint32_t(std::min<int64_t>(frames, (-drift) * tr.rate / 10000000));
        data += size_t(skip) * 2;
        frames -= skip;
    }
    if (frames == 0) return;
    WritePcm(tr, data, frames, tr.next);
    tr.next += int64_t(frames) * 10000000 / tr.rate;
}

bool Recorder::WriteClip(const std::wstring& path, const EncoderConfig& cfg, const std::vector<PacketPtr>& video,
                         const std::vector<AudioChunk>& audio)
{
    ComInit com;
    m_basePath = path;
    {
        std::lock_guard lock(m_errorMutex);
        m_path = path;
    }
    m_cfg = cfg;
    m_tracks.clear();
    if (!audio.empty()) {
        Track t;
        t.pending.assign(audio.begin(), audio.end());
        m_tracks.push_back(std::move(t));
    }
    size_t first = 0;
    while (first < video.size() && !video[first]->keyframe) ++first;
    const bool opened = first < video.size() && OpenSink(*video[first]);
    for (auto& t : m_tracks) t.pending.clear();
    if (!opened) return false;
    // Interleave audio and video by time so the MP4 sink gets a sensible order.
    size_t a = 0;
    for (size_t v = first; v < video.size(); ++v) {
        if (!m_tracks.empty())
            while (a < audio.size() && audio[a].time <= video[v]->time) WriteAudio(m_tracks[0], audio[a++]);
        WriteVideo(video[v]);
    }
    FlushVideo();
    if (!m_tracks.empty())
        while (a < audio.size()) WriteAudio(m_tracks[0], audio[a++]);
    HRESULT hr = m_writer->Finalize();
    m_writer.Reset();
    m_writing = false;
    m_tracks.clear();
    if (FAILED(hr)) {
        Fail(L"Finalizing the clip", hr);
        return false;
    }
    LOGI(L"Clip saved: %s (%.1f s)", path.c_str(), m_seconds.load());
    return true;
}

void Recorder::ThreadMain()
{
    ComInit com;
    SetCurrentThreadName(L"Recorder");
    SetThreadEfficiency(true);   // writing the file: a few ms behind costs nothing (packets queue up)
    const int64_t split = int64_t(m_splitSeconds) * 10000000;
    for (;;) {
        std::deque<PacketPtr> packets;
        bool stop;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(10), [&] { return m_stop || !m_packets.empty(); });
            packets.swap(m_packets);
            stop = m_stop;
        }
        RefreshThreadEfficiency();
        // Audio first (it lags video slightly), then video.
        for (auto& tr : m_tracks) {
            AudioChunk c;
            while (tr.tap && tr.tap->Pop(c)) {
                if (m_writer) {
                    WriteAudio(tr, c);
                    // The last second is kept for a new part (timed split, or the encoder restarted).
                    tr.recent.push_back(std::move(c));
                    while (tr.recent.size() > 1 && tr.recent.back().time - tr.recent.front().time > 10000000) tr.recent.pop_front();
                } else {
                    tr.pending.push_back(std::move(c));
                    while (tr.pending.size() > 300) tr.pending.pop_front();
                }
            }
        }
        for (const auto& p : packets) {
            const bool open = !m_pauses.empty() && m_pauses.back().second < 0;
            if (m_pauseWanted && !open) {   // pause from this frame on (the held frame ends here)
                FlushVideo(p->time);
                m_pauses.push_back({p->time, -1});
            }
            if (!m_pauseWanted && open && p->keyframe) m_pauses.back().second = p->time;   // resume on a keyframe
            if (!m_pauses.empty() && m_pauses.back().second < 0) continue;   // paused (or waiting for the keyframe)
            const int64_t fileTime = p->time - Shift(p->time) - m_t0;
            if (m_writer && split > 0 && !p->keyframe && !m_kfRequested && fileTime >= split) {
                // Split on time instead of waiting up to a whole GOP.
                std::lock_guard lock(m_mutex);
                if (m_kfEncoder) m_kfEncoder->RequestKeyframe();
                m_kfRequested = true;
            }
            // A keyframe whose parameter sets differ from the file's (the encoder was restarted, e.g. Auto
            // chose a faster preset) can't go into the same MP4: continue in a new part, seamlessly.
            bool psChanged = false;
            if (m_writer && p->keyframe) {
                const std::vector<uint8_t> ps = m_cfg.hevc ? h264::ExtractParameterSetsHevc(p->data.data(), p->data.size())
                                                           : h264::ExtractParameterSets(p->data.data(), p->data.size());
                psChanged = !ps.empty() && ps != m_filePs;
                if (psChanged) LOGI(L"Recorder: the encoder's stream headers changed; continuing in a new part");
            }
            if (m_writer && p->keyframe && (psChanged || (split > 0 && fileTime >= split))) {
                m_kfRequested = false;
                // Seamless split: the next part starts on this keyframe; recent audio is replayed into it
                // (WriteAudio drops everything before the new start).
                FlushVideo(p->time);
                FinishFile();
                const int part = m_part + 1;
                {
                    std::lock_guard lock(m_errorMutex);
                    m_path = PartPath(part);
                }
                m_part = part;
                if (psChanged) ++m_headerParts;
                if (!OpenSink(*p)) {
                    stop = true;
                    break;
                }
                for (auto& tr : m_tracks)
                    for (const auto& c : tr.recent) WriteAudio(tr, c);
            }
            if (!m_writer) {
                if (!p->keyframe) continue;   // a file has to start on a keyframe
                if (!OpenSink(*p)) {
                    stop = true;
                    break;
                }
                for (auto& tr : m_tracks) {
                    for (const auto& c : tr.pending) WriteAudio(tr, c);
                    tr.pending.clear();
                }
            }
            WriteVideo(p);
        }
        if (stop) break;
    }
    FinishFile();
    m_writing = false;
    m_active = false;
}

} // namespace cc
