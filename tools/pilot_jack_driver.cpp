#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <jack/jack.h>
#include <jack/midiport.h>

struct BbtPoint final {
    uint64_t frame = 0;
    double bpm = 0.0;
    double beats = 0.0;
    int32_t bar = 0;
    int32_t beat = 0;
    double tick = 0.0;
};

struct BbtTransition final {
    BbtPoint before{};
    BbtPoint after{};
};

struct Driver final {
    jack_client_t* client = nullptr; jack_port_t *ao1=nullptr,*ao2=nullptr,*ai1=nullptr,*ai2=nullptr,*mo=nullptr;
    std::string mode; std::string wav_path; double sr=48000.0; uint32_t quantum=1024; std::atomic<bool> done{false};
    std::vector<float> rec_l, rec_r; std::size_t rec_frames=0; uint64_t event_base=4096, play_on=196096, play_off=292096; uint64_t last_frame=0;
    uint64_t dynamic_a=216000, dynamic_b=272000; uint64_t replace_on=144000, replace_off=240000; uint64_t origin_frame=0; bool origin_set=false; uint64_t timeline=0;
    std::array<BbtTransition, 8> bbt_transitions{}; std::size_t bbt_transition_count=0;
    BbtPoint last_bbt{}; bool have_last_bbt=false; bool bbt_monotonic=true; bool bbt_coherent=true; uint64_t bbt_valid_callbacks=0;
};
static double bpm_at(const Driver* d, uint64_t frame) {
    if (d->mode == "elastic60") return frame >= d->play_on ? 60.0 : 120.0;
    if (d->mode == "elastic120") return 120.0;
    if (d->mode == "elastic240") return frame >= d->play_on ? 240.0 : 120.0;
    if (d->mode == "dynamic") {
        if (frame < d->dynamic_a) return 120.0;
        if (frame < d->dynamic_b) return 90.0;
        return 150.0;
    }
    return 120.0;
}
static double source_hz(const Driver* d, uint64_t frame) {
    return d->mode == "slotreplace" && frame >= d->replace_on ? 660.0 : 440.0;
}
static double beats_to(Driver* d, uint64_t frame) {
    double beats = 0.0;
    uint64_t cur = 0;
    while (cur < frame) {
        uint64_t end = frame;
        if (d->mode == "dynamic") {
            if (cur < d->dynamic_a) end = std::min(end, d->dynamic_a);
            else if (cur < d->dynamic_b) end = std::min(end, d->dynamic_b);
        } else if (d->mode == "elastic60" || d->mode == "elastic240") {
            if (cur < d->play_on) end = std::min(end, d->play_on);
        }
        beats += static_cast<double>(end - cur) * bpm_at(d, cur) / (d->sr * 60.0);
        cur = end;
    }
    return beats;
}
static double expected_beats(const Driver* d, uint64_t frame) {
    auto segment = [d](uint64_t begin, uint64_t end, double bpm) {
        return static_cast<double>(end - begin) * bpm / (d->sr * 60.0);
    };
    if (d->mode == "dynamic") {
        const uint64_t a = std::min(frame, d->dynamic_a);
        const uint64_t b = std::min(frame, d->dynamic_b);
        double result = segment(0, a, 120.0);
        if (frame > d->dynamic_a) result += segment(d->dynamic_a, b, 90.0);
        if (frame > d->dynamic_b) result += segment(d->dynamic_b, frame, 150.0);
        return result;
    }
    if (d->mode == "elastic60" || d->mode == "elastic240") {
        const uint64_t cut = std::min(frame, d->play_on);
        double result = segment(0, cut, 120.0);
        if (frame > d->play_on) result += segment(d->play_on, frame, d->mode == "elastic60" ? 60.0 : 240.0);
        return result;
    }
    return segment(0, frame, 120.0);
}
static void timebase(jack_transport_state_t, jack_nframes_t, jack_position_t* p, int, void* arg) {
    auto* d=static_cast<Driver*>(arg); const uint64_t f=d->timeline; const double bpm=bpm_at(d,f); const double beats=beats_to(d,f);
    const int32_t bar=static_cast<int32_t>(beats/4.0)+1; const int32_t beat=static_cast<int32_t>(std::fmod(beats,4.0))+1; const double tick=std::fmod(beats,1.0)*192.0;
    const BbtPoint point{f,bpm,beats,bar,beat,tick};
    ++d->bbt_valid_callbacks;
    if (d->have_last_bbt) {
        if (point.beats + 1e-9 < d->last_bbt.beats) d->bbt_monotonic=false;
        const double reconstructed=(static_cast<double>(point.bar-1)*4.0)+(point.beat-1.0)+(point.tick/192.0);
        if (std::abs(reconstructed-point.beats)>1e-6) d->bbt_coherent=false;
        if (std::abs(point.bpm-d->last_bbt.bpm)>1e-9 && d->bbt_transition_count<d->bbt_transitions.size()) {
            d->bbt_transitions[d->bbt_transition_count++] = BbtTransition{d->last_bbt,point};
        }
    }
    d->last_bbt=point; d->have_last_bbt=true;
    p->frame=static_cast<jack_nframes_t>(f); p->valid=JackPositionBBT; p->bar=bar; p->beat=static_cast<double>(beat); p->tick=tick;
    p->ticks_per_beat=192.0; p->beats_per_bar=4.0; p->beat_type=4.0; p->beats_per_minute=bpm; p->bar_start_tick=std::floor(beats/4.0)*768.0;
}

static bool validate_bbt(const Driver& d) {
    const std::size_t expected_changes = d.mode == "dynamic" ? 2 : ((d.mode == "elastic60" || d.mode == "elastic240") ? 1 : 0);
    bool ok = d.have_last_bbt && d.bbt_monotonic && d.bbt_coherent && d.bbt_transition_count == expected_changes;
    for (std::size_t i=0; i<d.bbt_transition_count; ++i) {
        const auto& t=d.bbt_transitions[i]; const double expected_delta=expected_beats(&d,t.after.frame)-expected_beats(&d,t.before.frame); const double actual_delta=t.after.beats-t.before.beats; const double error=actual_delta-expected_delta;
        std::cout << "BBT_TRANSITION mode=" << d.mode << " before_frame=" << t.before.frame << " before_bpm=" << t.before.bpm << " before_beats=" << t.before.beats
                  << " after_frame=" << t.after.frame << " after_bpm=" << t.after.bpm << " after_beats=" << t.after.beats
                  << " expected_delta=" << expected_delta << " actual_delta=" << actual_delta << " error=" << error << "\n";
        if (std::abs(t.before.bpm-bpm_at(&d,t.before.frame))>1e-9 || std::abs(t.after.bpm-bpm_at(&d,t.after.frame))>1e-9 || std::abs(error)>1e-6) ok=false;
    }
    std::cout << "BBT_VALIDATION mode=" << d.mode << " valid_callbacks=" << d.bbt_valid_callbacks << " transitions=" << d.bbt_transition_count
              << " expected_transitions=" << expected_changes << " monotonic=" << (d.bbt_monotonic?1:0) << " coherent=" << (d.bbt_coherent?1:0) << " result=" << (ok?"PASS":"FAIL") << "\n";
    return ok;
}
static int process(jack_nframes_t n, void* arg) {
    auto* d=static_cast<Driver*>(arg); auto* l=static_cast<float*>(jack_port_get_buffer(d->ao1,n)); auto* r=static_cast<float*>(jack_port_get_buffer(d->ao2,n));
    auto* il=static_cast<const float*>(jack_port_get_buffer(d->ai1,n)); auto* ir=static_cast<const float*>(jack_port_get_buffer(d->ai2,n)); void* mb=jack_port_get_buffer(d->mo,n); jack_midi_clear_buffer(mb);
    jack_position_t p{}; jack_transport_query(d->client,&p); const uint64_t base=p.frame; const uint64_t rel=d->timeline; d->last_frame=rel; d->timeline += n;
    for (jack_nframes_t i=0;i<n;++i) { const double x=0.2*std::sin(2.0*M_PI*source_hz(d,rel+i)*static_cast<double>(base+i)/d->sr); l[i]=static_cast<float>(x); r[i]=static_cast<float>(x); if (d->rec_frames<d->rec_l.size()) { d->rec_l[d->rec_frames]=il[i]; d->rec_r[d->rec_frames]=ir[i]; ++d->rec_frames; } }
    if (d->mode != "seq66") {
        auto emit=[&](uint64_t at,uint8_t st,uint8_t note,uint8_t vel){ if (at>=rel && at<rel+n) { uint8_t msg[3]={st,note,vel}; jack_midi_event_write(mb,static_cast<jack_nframes_t>(at-rel),msg,3); std::cout.flush(); } };
        if (d->mode == "slotreplace") {
            emit(d->event_base,0x90,36,100); emit(d->event_base+96000,0x80,36,0);
            emit(d->replace_on,0x90,36,100); emit(d->replace_off,0x80,36,0);
            emit(d->play_on,0x91,36,100); emit(d->play_off,0x81,36,0);
        } else {
            emit(d->event_base,0x90,36,100); emit(d->event_base+96000,0x80,36,0);
            emit(d->play_on,0x91,36,100); emit(d->play_off,0x81,36,0);
        }
        if (rel > d->play_off + 48000) d->done.store(true);
    } else if (rel > 360000) {
        d->done.store(true);
    }
    return 0;
}
static void wav(const std::string& path,const std::vector<float>& l,const std::vector<float>& r,std::size_t n,uint32_t sr) {
    std::ofstream f(path,std::ios::binary); const uint32_t data=static_cast<uint32_t>(n*4*2), fact_size=4, riff=48+data, fmt=16, audio=3, channels=2, rate=sr, bytes=rate*channels*4, align=channels*4, bits=32, frames=static_cast<uint32_t>(n);
    f.write("RIFF",4); f.write(reinterpret_cast<const char*>(&riff),4); f.write("WAVEfmt ",8); f.write(reinterpret_cast<const char*>(&fmt),4); f.write(reinterpret_cast<const char*>(&audio),2); f.write(reinterpret_cast<const char*>(&channels),2); f.write(reinterpret_cast<const char*>(&rate),4); f.write(reinterpret_cast<const char*>(&bytes),4); f.write(reinterpret_cast<const char*>(&align),2); f.write(reinterpret_cast<const char*>(&bits),2); f.write("fact",4); f.write(reinterpret_cast<const char*>(&fact_size),4); f.write(reinterpret_cast<const char*>(&frames),4); f.write("data",4); f.write(reinterpret_cast<const char*>(&data),4);
    for(std::size_t i=0;i<n;++i){f.write(reinterpret_cast<const char*>(&l[i]),4);f.write(reinterpret_cast<const char*>(&r[i]),4);} }
int main(int argc,char**argv){ Driver d; d.mode=argc>1?argv[1]:"rec120"; d.wav_path=argc>2?argv[2]:"output.wav"; const bool seq66_mode=d.mode=="seq66"; if(d.mode=="elastic60")d.play_off=d.play_on+240000; else if(d.mode=="elastic120")d.play_off=d.play_on+144000; else if(d.mode=="elastic240")d.play_off=d.play_on+96000; else if(d.mode=="dynamic"){d.play_off=484096;d.dynamic_a=d.play_on+24000;d.dynamic_b=d.play_on+80000;} else if(d.mode=="slotreplace"){d.replace_on=d.event_base+144000;d.replace_off=d.event_base+240000;d.play_on=d.event_base+336000;d.play_off=d.event_base+432000;}
    jack_status_t st{}; d.client=jack_client_open("pilot_jack_driver",JackNoStartServer,&st); if(!d.client){std::cerr<<"driver jack open failed\n";return 1;} d.sr=jack_get_sample_rate(d.client);d.quantum=jack_get_buffer_size(d.client); const std::size_t cap=static_cast<std::size_t>(d.sr*16);d.rec_l.resize(cap);d.rec_r.resize(cap);
    d.ao1=jack_port_register(d.client,"audio_out_1",JACK_DEFAULT_AUDIO_TYPE,JackPortIsOutput,0);d.ao2=jack_port_register(d.client,"audio_out_2",JACK_DEFAULT_AUDIO_TYPE,JackPortIsOutput,0);d.ai1=jack_port_register(d.client,"audio_in_1",JACK_DEFAULT_AUDIO_TYPE,JackPortIsInput,0);d.ai2=jack_port_register(d.client,"audio_in_2",JACK_DEFAULT_AUDIO_TYPE,JackPortIsInput,0);d.mo=jack_port_register(d.client,"midi_out",JACK_DEFAULT_MIDI_TYPE,JackPortIsOutput,0); jack_set_process_callback(d.client,process,&d); if(!seq66_mode && jack_set_timebase_callback(d.client,1,timebase,&d)!=0){std::cerr<<"driver could not become timebase\n";return 2;} if(jack_activate(d.client)!=0)return 3;
    for(int i=0;i<50;++i){ int rc1=jack_connect(d.client,jack_port_name(d.ao1),"live-sampler:audio_in_1"); int rc2=jack_connect(d.client,jack_port_name(d.ao2),"live-sampler:audio_in_2"); int rc3=jack_connect(d.client,"live-sampler:audio_out_1",jack_port_name(d.ai1)); int rc4=jack_connect(d.client,"live-sampler:audio_out_2",jack_port_name(d.ai2)); int rc5=jack_connect(d.client,jack_port_name(d.mo),"live-sampler:midi_in"); if(i==0) std::cerr<<"connect_rc="<<rc1<<","<<rc2<<","<<rc3<<","<<rc4<<","<<rc5<<"\n"; if(rc1==0 || rc1==17) break; std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    if(!seq66_mode){ jack_transport_locate(d.client, 0); jack_transport_start(d.client); } auto start=std::chrono::steady_clock::now(); auto last_report=start; while(!d.done.load()){ std::this_thread::sleep_for(std::chrono::milliseconds(50)); auto now=std::chrono::steady_clock::now(); if(now-last_report>std::chrono::seconds(1)){std::cerr<<"driver_frame="<<d.last_frame<<"\n";last_report=now;} if(now-start>std::chrono::seconds(30)){std::cerr<<"driver timeout\n";break;} } if(!seq66_mode) jack_transport_stop(d.client); jack_deactivate(d.client); const bool bbt_ok=validate_bbt(d); wav(d.wav_path,d.rec_l,d.rec_r,d.rec_frames,static_cast<uint32_t>(d.sr)); std::cout<<"DRIVER mode="<<d.mode<<" sample_rate="<<d.sr<<" quantum="<<d.quantum<<" captured_frames="<<d.rec_frames<<" output="<<d.wav_path<<"\n"; jack_client_close(d.client); return bbt_ok?0:4; }
