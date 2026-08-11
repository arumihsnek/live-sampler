#include "jack_client.hpp"
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

static JackClient* g_client = nullptr;
static void signal_handler(int) { if (g_client != nullptr) g_client->request_stop(); }

int main(int argc, char** argv) {
    double max_capture = 30.0;
    uint32_t voices = 8;
    int rec_channel = 1;
    int play_channel = 2;
    std::string name = "live-sampler";
    for (int i = 1; i < argc; ++i) {
        const std::string a(argv[i]);
        auto next = [&](const char* flag) -> const char* { if (i + 1 >= argc) { std::cerr << flag << " needs a value\n"; std::exit(2); } return argv[++i]; };
        if (a == "--rec-channel") rec_channel = std::atoi(next("--rec-channel"));
        else if (a == "--play-channel") play_channel = std::atoi(next("--play-channel"));
        else if (a == "--max-capture-seconds") max_capture = std::atof(next("--max-capture-seconds"));
        else if (a == "--voices") voices = static_cast<uint32_t>(std::atoi(next("--voices")));
        else if (a == "--name") name = next("--name");
        else if (a == "--help") { std::cout << "live-sampler --rec-channel 1 --play-channel 2 --max-capture-seconds 30 --voices 8\n"; return 0; }
        else { std::cerr << "unknown option: " << a << '\n'; return 2; }
    }
    if (rec_channel < 1 || rec_channel > 16 || play_channel < 1 || play_channel > 16 || voices == 0 || voices > 8 || max_capture <= 0.0) return 2;
    try {
        JackClient client(name, max_capture, voices, rec_channel, play_channel);
        g_client = &client;
        std::signal(SIGINT, signal_handler); std::signal(SIGTERM, signal_handler);
        if (!client.activate()) return 1;
        std::cout << "live-sampler active; ports: " << name << ":audio_in_1 " << name << ":audio_in_2 " << name << ":audio_out_1 " << name << ":audio_out_2 " << name << ":midi_in\n";
        client.run_until_stopped();
        client.print_diagnostics();
    } catch (const std::exception& e) { std::cerr << "live-sampler: " << e.what() << '\n'; return 1; }
    return 0;
}
