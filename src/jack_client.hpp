#pragma once
#include "sampler_engine.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <jack/jack.h>

class JackClient final {
public:
    JackClient(const std::string& name, double max_capture_seconds, uint32_t voices,
               int rec_channel, int play_channel);
    ~JackClient();
    bool activate();
    void run_until_stopped();
    void request_stop() noexcept;
    void print_diagnostics() const;
    SamplerEngine& engine() noexcept { return *engine_; }
private:
    static int process_cb(jack_nframes_t nframes, void* arg) noexcept;
    static void shutdown_cb(void* arg) noexcept;
    int process(jack_nframes_t nframes) noexcept;
    jack_client_t* client_ = nullptr;
    jack_port_t* in_left_ = nullptr;
    jack_port_t* in_right_ = nullptr;
    jack_port_t* out_left_ = nullptr;
    jack_port_t* out_right_ = nullptr;
    jack_port_t* midi_in_ = nullptr;
    std::unique_ptr<SamplerEngine> engine_;
    std::atomic<bool> stop_{false};
    int rec_channel_;
    int play_channel_;
};
