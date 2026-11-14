#include "player.h"

#include <audioclient.h>
#include <mfapi.h>
#include <mferror.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace {

const GUID KS_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

std::wstring codecName(const GUID& g) {
    if (IsEqualGUID(g, MFAudioFormat_Dolby_AC3)) return L"AC-3";
    switch (g.Data1) {
        case 0x1610: return L"AAC";
        case 0x0055: return L"MP3";
        case 0xa7fb87af: return L"E-AC-3";
        case 0x0008: return L"DTS";
        case 0xF1AC: return L"FLAC";
        case 0x704F: return L"Opus";
        case 0x566f: return L"Vorbis";
        case 0x0161: case 0x0162: case 0x0163: return L"WMA";
        case 0x0001: return L"PCM";
        case 0x0003: return L"PCM float";
        default: return L"audio";
    }
}

std::wstring langName(std::wstring c) {
    std::transform(c.begin(), c.end(), c.begin(), ::towlower);
    struct L { const wchar_t* k; const wchar_t* n; };
    static const L t[] = {{L"fr", L"Français"}, {L"fra", L"Français"}, {L"fre", L"Français"}, {L"en", L"Anglais"}, {L"eng", L"Anglais"},
        {L"de", L"Allemand"}, {L"deu", L"Allemand"}, {L"ger", L"Allemand"}, {L"es", L"Espagnol"}, {L"spa", L"Espagnol"},
        {L"it", L"Italien"}, {L"ita", L"Italien"}, {L"pt", L"Portugais"}, {L"por", L"Portugais"}, {L"ja", L"Japonais"}, {L"jpn", L"Japonais"},
        {L"ru", L"Russe"}, {L"rus", L"Russe"}, {L"ar", L"Arabe"}, {L"ara", L"Arabe"}, {L"zh", L"Chinois"}, {L"zho", L"Chinois"},
        {L"chi", L"Chinois"}, {L"ko", L"Coréen"}, {L"kor", L"Coréen"}, {L"nl", L"Néerlandais"}, {L"nld", L"Néerlandais"}};
    for (auto& e : t) if (c == e.k) return e.n;
    if (c.empty() || c == L"und") return L"";
    std::transform(c.begin(), c.end(), c.begin(), ::towupper);
    return c;
}

std::wstring chLabel(int ch) {
    if (ch == 6) return L" 5.1";
    if (ch == 8) return L" 7.1";
    if (ch == 2) return L" stéréo";
    if (ch == 1) return L" mono";
    return L" " + std::to_wstring(ch) + L" canaux";
}

// Mélange vers la stéréo (ordre WAV : FL FR FC LFE BL BR SL SR)
void downmix(const float* in, size_t frames, int ch, std::vector<float>& out) {
    out.resize(frames * 2);
    const float k = 0.707f;
    for (size_t i = 0; i < frames; i++) {
        const float* f = in + i * ch;
        float l, r;
        if (ch == 1) { l = r = f[0]; }
        else if (ch == 3) { l = f[0] + k * f[2]; r = f[1] + k * f[2]; }
        else if (ch == 4) { l = f[0] + k * f[2]; r = f[1] + k * f[3]; }
        else if (ch == 5) { l = f[0] + k * f[2] + k * f[3]; r = f[1] + k * f[2] + k * f[4]; }
        else if (ch == 6) { l = f[0] + k * f[2] + k * f[4]; r = f[1] + k * f[2] + k * f[5]; }
        else { l = f[0] + k * f[2] + k * f[4] + k * f[6 % ch]; r = f[1] + k * f[2] + k * f[5] + k * f[7 % ch]; }
        float n = ch == 1 ? 1.0f : 1.0f / (1.0f + 0.707f + 0.707f);
        out[2 * i] = l * n; out[2 * i + 1] = r * n;
    }
}

// Compresseur de dynamique (DRC / mode nuit), détection liée sur tous les canaux
struct Comp {
    float env = 0;
    void process(float* x, size_t frames, int ch, int rate, Drc mode) {
        if (mode != Drc::Film && mode != Drc::Night) { env = 0; return; }
        float thr = mode == Drc::Night ? 0.0079f : 0.0501f;   // -42 dB / -26 dB
        float ratio = mode == Drc::Night ? 10.0f : 3.0f;
        float makeup = mode == Drc::Night ? 2.0f : 1.2f;
        float att = 1.0f - std::exp(-1.0f / (rate * (mode == Drc::Night ? 0.002f : 0.005f)));
        float rel = 1.0f - std::exp(-1.0f / (rate * (mode == Drc::Night ? 0.15f : 0.25f)));
        for (size_t i = 0; i < frames; i++) {
            float* f = x + i * ch, pk = 0;
            for (int c = 0; c < ch; c++) pk = std::max(pk, std::fabs(f[c]));
            env += (pk - env) * (pk > env ? att : rel);
            float g = makeup;
            if (env > thr) g *= std::pow(env / thr, 1.0f / ratio - 1.0f);
            for (int c = 0; c < ch; c++) f[c] *= g;
        }
    }
};

}  // namespace

Player::Player() {
    MFStartup(MF_VERSION);
    quit_ = false;
    audioT_ = std::thread([this] { audioLoop(); });
}

Player::~Player() {
    close();
    quit_ = true;
    cv_.notify_all();
    if (audioT_.joinable()) audioT_.join();
    MFShutdown();
}

void Player::setMix(ac3::Mix m) {
    if (m == ac3::Mix::Surround51 && devCh_ < 6) m = ac3::Mix::Stereo;
    mix_ = m;
}

void Player::close() {
    if (worker_.joinable()) {
        workerStop_ = true;
        cv_.notify_all();
        worker_.join();
        workerStop_ = false;
    }
    playing_ = false;
    rd_.Reset();
    tracks_.clear();
    rawData_.clear(); rawIdx_.clear();
    {
        std::lock_guard<std::mutex> l(mu_);
        vq_.clear(); aq_.clear(); aqRead_ = 0;
        haveBase_ = false; hasAudio_ = false; written_ = 0; padding_ = 0; flushAudio_ = true;
    }
    vs_ = -1; curTrack_ = -1; vw_ = vh_ = 0; dur_ = 0; open_ = false; raw_ = false; eos_ = false;
}

bool Player::configureVideo() {
    ComPtr<IMFMediaType> nt, ot, cur;
    if (FAILED(rd_->GetNativeMediaType(vs_, 0, &nt))) return false;
    if (FAILED(MFCreateMediaType(&ot))) return false;
    ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(rd_->SetCurrentMediaType(vs_, nullptr, ot.Get()))) return false;
    if (FAILED(rd_->GetCurrentMediaType(vs_, &cur))) return false;
    UINT32 w = 0, h = 0, n = 1, d = 1, st = 0;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(cur.Get(), MF_MT_PIXEL_ASPECT_RATIO, &n, &d);
    if (FAILED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &st))) st = (UINT32)(-(LONG)(w * 4));
    vw_ = (int)w; vh_ = (int)h; pn_ = n ? (int)n : 1; pd_ = d ? (int)d : 1; vstride_ = (LONG)st;
    return vw_ > 0 && vh_ > 0;
}

bool Player::configureAudio(int i) {
    const AudioTrack& t = tracks_[i];
    ComPtr<IMFMediaType> nt;
    if (FAILED(rd_->GetNativeMediaType(t.stream, 0, &nt))) return false;
    GUID sub = {};
    nt->GetGUID(MF_MT_SUBTYPE, &sub);
    ownAc3_ = false;
    if (IsEqualGUID(sub, MFAudioFormat_Dolby_AC3)) {
        // échantillons AC-3 bruts, décodés par notre décodeur (aucun codec Dolby requis dans Windows)
        if (SUCCEEDED(rd_->SetCurrentMediaType(t.stream, nullptr, nt.Get()))) { ownAc3_ = true; feeder_.reset(); return true; }
    }
    ComPtr<IMFMediaType> ot, cur;
    MFCreateMediaType(&ot);
    ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    ot->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(rd_->SetCurrentMediaType(t.stream, nullptr, ot.Get()))) return false;
    if (SUCCEEDED(rd_->GetCurrentMediaType(t.stream, &cur))) {
        nativeCh_ = (int)MFGetAttributeUINT32(cur.Get(), MF_MT_AUDIO_NUM_CHANNELS, 2);
        nativeRate_ = (int)MFGetAttributeUINT32(cur.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    }
    return true;
}

bool Player::open(const std::wstring& path, std::wstring& err) {
    close();
    size_t dot = path.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (ext == L"ac3" || ext == L"a52") {
        std::ifstream f(path, std::ios::binary);
        rawData_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        ac3::Header h;
        size_t p = 0;
        while (p + 8 <= rawData_.size()) {
            if (!ac3::Decoder::parseHeader(rawData_.data() + p, rawData_.size() - p, h) || h.bsid > 8) { p++; continue; }
            if (rawIdx_.empty()) rawFs_ = h.fs;
            rawIdx_.push_back(p); p += h.size;
        }
        if (rawIdx_.empty()) { err = L"Aucune trame AC-3 trouvée (E-AC-3 non pris en charge)."; return false; }
        raw_ = true; rawPos_ = 0;
        dur_ = rawIdx_.size() * 1536.0 / rawFs_;
        AudioTrack t; t.stream = -1; t.ownAc3 = true; t.label = L"Piste 1 — AC-3";
        tracks_.push_back(t); curTrack_ = 0; hasAudio_ = true;
    } else {
        ComPtr<IMFAttributes> a;
        MFCreateAttributes(&a, 2);
        a->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), a.Get(), &rd_);
        if (FAILED(hr)) { err = L"Format ou codec non reconnu par Windows (Media Foundation)."; return false; }
        rd_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        int vstream = -1, an = 0;
        for (DWORD i = 0;; i++) {
            ComPtr<IMFMediaType> t;
            hr = rd_->GetNativeMediaType(i, 0, &t);
            if (hr == MF_E_INVALIDSTREAMNUMBER) break;
            if (FAILED(hr)) continue;
            GUID major = {}, sub = {};
            t->GetGUID(MF_MT_MAJOR_TYPE, &major);
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (IsEqualGUID(major, MFMediaType_Video) && vstream < 0) vstream = (int)i;
            if (IsEqualGUID(major, MFMediaType_Audio)) {
                AudioTrack at; at.stream = (int)i;
                at.ownAc3 = IsEqualGUID(sub, MFAudioFormat_Dolby_AC3);
                UINT32 ch = MFGetAttributeUINT32(t.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
                std::wstring lang, name;
                PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(rd_->GetPresentationAttribute(i, MF_SD_LANGUAGE, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) lang = langName(pv.pwszVal);
                PropVariantClear(&pv);
                if (SUCCEEDED(rd_->GetPresentationAttribute(i, MF_SD_STREAM_NAME, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) name = pv.pwszVal;
                PropVariantClear(&pv);
                std::wstring s = L"Piste " + std::to_wstring(++an);
                if (!lang.empty()) s += L" — " + lang;
                if (!name.empty()) s += L" (" + name + L")";
                s += L" — " + codecName(sub);
                if (ch) s += chLabel((int)ch);
                at.label = s;
                tracks_.push_back(at);
            }
        }
        if (vstream >= 0) {
            vs_ = vstream;
            rd_->SetStreamSelection(vs_, TRUE);
            if (!configureVideo()) { rd_->SetStreamSelection(vs_, FALSE); vs_ = -1; }
        }
        for (size_t i = 0; i < tracks_.size() && curTrack_ < 0; i++) {
            rd_->SetStreamSelection(tracks_[i].stream, TRUE);
            if (configureAudio((int)i)) { curTrack_ = (int)i; hasAudio_ = true; }
            else rd_->SetStreamSelection(tracks_[i].stream, FALSE);
        }
        if (vs_ < 0 && curTrack_ < 0) { err = L"Aucune piste audio ou vidéo lisible (codec absent de Windows ?)."; rd_.Reset(); tracks_.clear(); return false; }
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(rd_->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv)) && pv.vt == VT_UI8)
            dur_ = (double)pv.uhVal.QuadPart / 1e7;
        PropVariantClear(&pv);
    }
    {
        std::lock_guard<std::mutex> l(mu_);
        flushLocked(0);
        aqCh_ = 2; aqRate_ = raw_ ? rawFs_ : 48000;
        audioReinit_ = true;
    }
    open_ = true; eos_ = false; workerStop_ = false;
    worker_ = std::thread([this] { workerLoop(); });
    return true;
}

void Player::flushLocked(double target) {
    vq_.clear(); aq_.clear(); aqRead_ = 0;
    haveBase_ = false; needFirst_ = true; written_ = 0; padding_ = 0; wallAcc_ = 0; flushAudio_ = true;
    seekPos_ = target; eos_ = false;
    std::fill(std::begin(streamEnded_), std::end(streamEnded_), false);
    wallStart_ = std::chrono::steady_clock::now();
}

double Player::clockLocked() {
    if (!haveBase_) return seekPos_;
    if (hasAudio_) {
        double played = written_ > padding_ ? (double)(written_ - padding_) / aqRate_ : 0;
        return base_ + played;
    }
    double e = playing_ ? std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart_).count() : 0;
    return base_ + wallAcc_ + e;
}

double Player::position() {
    std::lock_guard<std::mutex> l(mu_);
    double c = clockLocked();
    if (dur_ > 0 && c > dur_) c = dur_;
    return c < 0 ? 0 : c;
}

void Player::play() {
    if (!open_) return;
    std::lock_guard<std::mutex> l(mu_);
    if (!playing_) { playing_ = true; wallStart_ = std::chrono::steady_clock::now(); }
    cv_.notify_all();
}

void Player::pause() {
    std::lock_guard<std::mutex> l(mu_);
    if (playing_ && haveBase_ && !hasAudio_)
        wallAcc_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart_).count();
    playing_ = false;
}

void Player::seek(double sec) {
    if (!open_) return;
    if (sec < 0) sec = 0;
    if (dur_ > 0 && sec > dur_) sec = dur_;
    std::lock_guard<std::mutex> l(mu_);
    seekReq_ = sec;
    seekPos_ = sec; haveBase_ = false;
    cv_.notify_all();
}

void Player::selectTrack(int i) {
    if (!open_ || i < 0 || i >= (int)tracks_.size() || i == curTrack_) return;
    std::lock_guard<std::mutex> l(mu_);
    trackReq_ = i;
    cv_.notify_all();
}

bool Player::ended() {
    std::lock_guard<std::mutex> l(mu_);
    return open_ && eos_ && vq_.empty() && aqFrames() == 0;
}

bool Player::fetchFrame(std::vector<uint8_t>& px, int& w, int& h, bool& bottomUp) {
    std::lock_guard<std::mutex> l(mu_);
    if (vq_.empty()) return false;
    bool got = false;
    if (haveBase_ || !hasAudio_) {
        double c = clockLocked();
        while (!vq_.empty() && vq_.front().ts <= c + 0.012) {
            px.swap(vq_.front().px); vq_.pop_front(); got = true; needFirst_ = false;
        }
    }
    if (!got && needFirst_) { px.swap(vq_.front().px); vq_.pop_front(); got = true; needFirst_ = false; }
    if (got) { w = vw_; h = vh_; bottomUp = vstride_ < 0; cv_.notify_all(); }
    return got;
}

void Player::pushAudio(const float* pcm, size_t frames, int ch, int rate, double ts) {
    std::unique_lock<std::mutex> l(mu_);
    if (!haveBase_) { base_ = ts; haveBase_ = true; wallStart_ = std::chrono::steady_clock::now(); }
    if (ch != aqCh_ || rate != aqRate_) {
        if (aqFrames() == 0) { aqCh_ = ch; aqRate_ = rate; audioReinit_ = true; }
    }
    aq_.insert(aq_.end(), pcm, pcm + frames * ch);
}

void Player::handleAudio(IMFSample* s, double ts) {
    ComPtr<IMFMediaBuffer> mb;
    if (FAILED(s->ConvertToContiguousBuffer(&mb))) return;
    BYTE* p = nullptr; DWORD len = 0;
    if (FAILED(mb->Lock(&p, nullptr, &len))) return;
    if (ownAc3_) {
        feeder_.setMix(mix_);
        feeder_.setDrcScale(drc_ == (int)Drc::Off ? 0.0 : 1.0);
        std::vector<float> pcm;
        feeder_.push(p, len, pcm);
        mb->Unlock();
        if (!pcm.empty()) pushAudio(pcm.data(), pcm.size() / feeder_.channels(), feeder_.channels(), feeder_.sampleRate(), ts);
        return;
    }
    size_t frames = len / (sizeof(float) * (size_t)nativeCh_);
    if (nativeCh_ > 2 || nativeCh_ == 1) {
        std::vector<float> st;
        downmix((const float*)p, frames, nativeCh_, st);
        mb->Unlock();
        pushAudio(st.data(), frames, 2, nativeRate_, ts);
    } else {
        std::vector<float> copy((const float*)p, (const float*)p + frames * 2);
        mb->Unlock();
        pushAudio(copy.data(), frames, 2, nativeRate_, ts);
    }
}

void Player::handleVideo(IMFSample* s, double ts) {
    ComPtr<IMFMediaBuffer> mb;
    if (FAILED(s->ConvertToContiguousBuffer(&mb))) return;
    BYTE* p = nullptr; DWORD len = 0;
    if (FAILED(mb->Lock(&p, nullptr, &len))) return;
    size_t need = (size_t)vw_ * 4 * vh_;
    VFrame f;
    f.ts = ts;
    f.px.assign(p, p + std::min<size_t>(len, need));
    f.px.resize(need);
    mb->Unlock();
    std::unique_lock<std::mutex> l(mu_);
    // ne bloque que si l'audio est suffisamment alimenté (évite tout blocage mutuel)
    while (!workerStop_ && seekReq_ < 0 && trackReq_ < 0 && vq_.size() >= 8 && (!hasAudio_ || aqFrames() > (size_t)(aqRate_ / 8)))
        cv_.wait_for(l, std::chrono::milliseconds(10));
    if (seekReq_ >= 0 || trackReq_ >= 0) return;  // image périmée
    if (!hasAudio_ && !haveBase_) { base_ = ts; haveBase_ = true; wallStart_ = std::chrono::steady_clock::now(); }
    vq_.push_back(std::move(f));
}

void Player::handleRaw() {
    std::unique_lock<std::mutex> l(mu_);
    while (!workerStop_ && seekReq_ < 0 && trackReq_ < 0 && aqFrames() > (size_t)aqRate_) cv_.wait_for(l, std::chrono::milliseconds(10));
    if (workerStop_ || seekReq_ >= 0 || trackReq_ >= 0) return;
    l.unlock();
    if (rawPos_ >= rawIdx_.size()) { std::lock_guard<std::mutex> g(mu_); eos_ = true; return; }
    size_t last = std::min(rawPos_ + 8, rawIdx_.size());
    size_t from = rawIdx_[rawPos_], to = last < rawIdx_.size() ? rawIdx_[last] : rawData_.size();
    double ts = rawPos_ * 1536.0 / rawFs_;
    feeder_.setMix(mix_);
    feeder_.setDrcScale(drc_ == (int)Drc::Off ? 0.0 : 1.0);
    std::vector<float> pcm;
    feeder_.push(rawData_.data() + from, to - from, pcm);
    rawPos_ = last;
    if (!pcm.empty()) pushAudio(pcm.data(), pcm.size() / feeder_.channels(), feeder_.channels(), feeder_.sampleRate(), ts);
}

void Player::doSeek(double sec) {
    if (raw_) {
        size_t k = (size_t)(sec * rawFs_ / 1536.0);
        k = k > 2 ? k - 2 : 0;  // quelques trames de recouvrement pour l'IMDCT
        rawPos_ = std::min(k, rawIdx_.size());
        feeder_.reset();
        std::lock_guard<std::mutex> l(mu_);
        flushLocked(sec);
        return;
    }
    rd_->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS);
    PROPVARIANT pv; PropVariantInit(&pv);
    pv.vt = VT_I8; pv.hVal.QuadPart = (LONGLONG)(sec * 1e7);
    rd_->SetCurrentPosition(GUID_NULL, pv);
    feeder_.reset();
    std::lock_guard<std::mutex> l(mu_);
    flushLocked(sec);
}

void Player::doSelect(int i) {
    double pos = position();
    if (!raw_) {
        rd_->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS);
        rd_->SetStreamSelection(tracks_[curTrack_].stream, FALSE);
        rd_->SetStreamSelection(tracks_[i].stream, TRUE);
        if (configureAudio(i)) curTrack_ = i;
        else {  // piste illisible : on revient à la précédente
            rd_->SetStreamSelection(tracks_[i].stream, FALSE);
            rd_->SetStreamSelection(tracks_[curTrack_].stream, TRUE);
            configureAudio(curTrack_);
        }
    }
    doSeek(pos);
}

void Player::workerLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!workerStop_) {
        double sr; int tr;
        { std::lock_guard<std::mutex> l(mu_); sr = seekReq_; tr = trackReq_; }
        if (tr >= 0) { { std::lock_guard<std::mutex> l(mu_); trackReq_ = -1; } doSelect(tr); continue; }
        if (sr >= 0) { { std::lock_guard<std::mutex> l(mu_); seekReq_ = -1; } doSeek(sr); continue; }
        if (eos_) {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait_for(l, std::chrono::milliseconds(20));
            continue;
        }
        if (raw_) { handleRaw(); continue; }
        DWORD si = 0, fl = 0; LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        HRESULT hr = rd_->ReadSample((DWORD)MF_SOURCE_READER_ANY_STREAM, 0, &si, &fl, &ts, &s);
        if (FAILED(hr) || (fl & MF_SOURCE_READERF_ERROR)) { std::lock_guard<std::mutex> l(mu_); eos_ = true; continue; }
        if (fl & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            if ((int)si == vs_) configureVideo();
            else if (curTrack_ >= 0 && (int)si == tracks_[curTrack_].stream && !ownAc3_) {
                ComPtr<IMFMediaType> cur;
                if (SUCCEEDED(rd_->GetCurrentMediaType(si, &cur))) {
                    nativeCh_ = (int)MFGetAttributeUINT32(cur.Get(), MF_MT_AUDIO_NUM_CHANNELS, 2);
                    nativeRate_ = (int)MFGetAttributeUINT32(cur.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
                }
            }
        }
        if (fl & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (si < 64) streamEnded_[si] = true;
            bool all = (vs_ < 0 || streamEnded_[vs_]) && (curTrack_ < 0 || streamEnded_[tracks_[curTrack_].stream]);
            if (all) { std::lock_guard<std::mutex> l(mu_); eos_ = true; }
        }
        if (!s) continue;
        double t = ts / 1e7;
        if ((int)si == vs_) handleVideo(s.Get(), t);
        else if (curTrack_ >= 0 && (int)si == tracks_[curTrack_].stream) {
            {
                std::unique_lock<std::mutex> l(mu_);
                while (!workerStop_ && seekReq_ < 0 && trackReq_ < 0 && aqFrames() > (size_t)aqRate_ && (vs_ < 0 || vq_.size() > 2))
                    cv_.wait_for(l, std::chrono::milliseconds(10));
                if (seekReq_ >= 0 || trackReq_ >= 0) continue;
            }
            handleAudio(s.Get(), t);
        }
    }
    CoUninitialize();
}

void Player::audioLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> en;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    ComPtr<IAudioClient> cl;
    ComPtr<IAudioRenderClient> rc;
    UINT32 bufFrames = 0;
    int curRate = 0, curCh = 0;
    bool running = false;
    Comp comp;
    std::vector<float> tmp;
    while (!quit_) {
        int wr, wc; bool reinit, flush;
        {
            std::lock_guard<std::mutex> l(mu_);
            wr = aqRate_; wc = aqCh_; reinit = audioReinit_; flush = flushAudio_;
            audioReinit_ = false; flushAudio_ = false;
        }
        if (en && (!cl || reinit || wr != curRate || wc != curCh)) {
            if (cl && running) cl->Stop();
            running = false; rc.Reset(); cl.Reset();
            ComPtr<IMMDevice> dev;
            if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev)) &&
                SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)cl.GetAddressOf()))) {
                WAVEFORMATEX* mf = nullptr;
                if (SUCCEEDED(cl->GetMixFormat(&mf)) && mf) { devCh_ = mf->nChannels; CoTaskMemFree(mf); }
                WAVEFORMATEXTENSIBLE wf = {};
                wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                wf.Format.nChannels = (WORD)wc;
                wf.Format.nSamplesPerSec = (DWORD)wr;
                wf.Format.wBitsPerSample = 32;
                wf.Format.nBlockAlign = (WORD)(wc * 4);
                wf.Format.nAvgBytesPerSec = (DWORD)(wr * wc * 4);
                wf.Format.cbSize = 22;
                wf.Samples.wValidBitsPerSample = 32;
                wf.dwChannelMask = wc == 6 ? 0x60F : wc == 2 ? 0x3 : 0x4;
                wf.SubFormat = KS_FLOAT;
                HRESULT hr = cl->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                            2000000, 0, &wf.Format, nullptr);
                if (SUCCEEDED(hr) && SUCCEEDED(cl->GetBufferSize(&bufFrames)) && SUCCEEDED(cl->GetService(IID_PPV_ARGS(&rc)))) { curRate = wr; curCh = wc; }
                else { rc.Reset(); cl.Reset(); curRate = curCh = 0; }
            }
            flush = true;
        }
        if (cl && flush) { cl->Stop(); cl->Reset(); running = false; comp.env = 0; }
        bool want = playing_ && hasAudio_;
        if (cl && rc) {
            if (want && !running) { cl->Start(); running = true; }
            else if (!want && running) { cl->Stop(); running = false; }
            if (running) {
                UINT32 pad = 0;
                if (SUCCEEDED(cl->GetCurrentPadding(&pad))) {
                    std::lock_guard<std::mutex> l(mu_);
                    padding_ = pad;
                    UINT32 avail = bufFrames - pad;
                    size_t n = std::min<size_t>(avail, aqFrames());
                    if (n > 0 && aqCh_ == curCh && aqRate_ == curRate) {
                        BYTE* data = nullptr;
                        if (SUCCEEDED(rc->GetBuffer((UINT32)n, &data))) {
                            float* o = (float*)data;
                            std::memcpy(o, aq_.data() + aqRead_, n * curCh * sizeof(float));
                            comp.process(o, n, curCh, curRate, (Drc)drc_.load());
                            float v = vol_;
                            for (size_t i = 0; i < n * curCh; i++) { float x = o[i] * v; o[i] = x > 1.f ? 1.f : x < -1.f ? -1.f : x; }
                            rc->ReleaseBuffer((UINT32)n, 0);
                            aqRead_ += n * curCh;
                            written_ += n;
                            padding_ = pad;
                            if (aqRead_ > (1u << 20)) { aq_.erase(aq_.begin(), aq_.begin() + (std::ptrdiff_t)aqRead_); aqRead_ = 0; }
                            cv_.notify_all();
                        }
                    }
                }
            }
        }
        std::unique_lock<std::mutex> l(mu_);
        cv_.wait_for(l, std::chrono::milliseconds(5));
    }
    if (cl && running) cl->Stop();
    CoUninitialize();
}
