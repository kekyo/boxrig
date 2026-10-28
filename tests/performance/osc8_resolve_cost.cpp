/* Consumer integration diagnostic. Build against one elder-terms checkout and
 * its matching SDK; the consumer sources and dependency references are read-only. */
#include "terminal-links.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace elder_terms;

struct Observation {
    std::size_t matchers = 0;
    std::size_t prepared_inputs = 0;
    std::size_t prepared_bytes = 0;
    std::size_t work = 0;
};

static Observation observation;

#ifdef BOXRIG_OBSERVE_OSC8
extern "C" boxrig_status __real_boxrig_regex_matcher_create(
    boxrig_regex_program *, const uint8_t *, std::size_t, std::size_t,
    std::size_t *, boxrig_regex_matcher **);
extern "C" boxrig_status __wrap_boxrig_regex_matcher_create(
    boxrig_regex_program *program, const uint8_t *input, std::size_t length,
    std::size_t start, std::size_t *error, boxrig_regex_matcher **matcher)
{
    observation.matchers++;
    const auto status = __real_boxrig_regex_matcher_create(
        program, input, length, start, error, matcher);
    if (status == BOXRIG_STATUS_OK) {
        /* The public create contract validates UTF-8 and owns one input copy.
         * Count completed preparations, not unrelated heap allocations. */
        observation.prepared_inputs++;
        observation.prepared_bytes += length;
    }
    return status;
}

extern "C" boxrig_status __real_boxrig_regex_matcher_resume(
    boxrig_regex_matcher *, std::size_t, std::size_t *,
    boxrig_regex_progress *, boxrig_regex_match *);
extern "C" boxrig_status __wrap_boxrig_regex_matcher_resume(
    boxrig_regex_matcher *matcher, std::size_t budget, std::size_t *used,
    boxrig_regex_progress *progress, boxrig_regex_match *match)
{
    const auto status = __real_boxrig_regex_matcher_resume(
        matcher, budget, used, progress, match);
    if (status == BOXRIG_STATUS_OK) observation.work += *used;
    return status;
}
#endif

static void expect(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

static std::vector<HyperlinkActionRule> rules_for(std::string_view scenario)
{
    auto rules = default_hyperlink_action_rules();
    std::erase_if(rules, [](const auto &rule) {
        return rule.recognition_source != HyperlinkRecognitionSource::osc8;
    });
    if (scenario == "custom") {
        const auto prototype = rules.front();
        rules.assign(3, prototype);
        const char *patterns[] = {"^https://a+z$", "^https://a+y$",
            "^(?<scheme>https?)://(?<body>a+)$"};
        for (std::size_t index = 0; index < rules.size(); index++) {
            rules[index].id = "custom-" + std::to_string(index);
            rules[index].pattern = patterns[index];
            rules[index].command = "custom-action-" + std::to_string(index);
        }
        rules.back().arguments = {"${scheme}://${body}"};
    }
    return rules;
}

static void measure(std::string_view scenario, std::size_t length,
    bool matching, std::string_view activity)
{
    auto config = boxrig_terminal_config_default();
    config.rows = 3;
    config.columns = 40;
    boxrig_terminal *terminal = nullptr;
    expect(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK, "terminal");
    const std::unique_ptr<boxrig_terminal, decltype(&boxrig_terminal_destroy)>
        terminal_owner(terminal, boxrig_terminal_destroy);
    boxrig_ui_model *model = nullptr;
    expect(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK, "model");
    const std::unique_ptr<boxrig_ui_model, decltype(&boxrig_ui_model_destroy)>
        model_owner(model, boxrig_ui_model_destroy);
    boxrig_ui_view *view = nullptr;
    const auto view_config = boxrig_ui_view_config_default();
    expect(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK, "view");
    const std::unique_ptr<boxrig_ui_view, decltype(&boxrig_ui_view_destroy)>
        view_owner(view, boxrig_ui_view_destroy);
    expect(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK, "authority");
    boxrig_ui_allocation_input allocation{};
    allocation.allocation_width = 320 * BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = 30 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = 8 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = 10 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    boxrig_ui_layout_result layout{};
    expect(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK, "allocation");
    const auto feed = [&](const std::string &input) {
        std::size_t consumed = 0;
        expect(boxrig_terminal_feed(terminal,
            reinterpret_cast<const uint8_t *>(input.data()), input.size(), &consumed) ==
            BOXRIG_STATUS_OK && consumed == input.size(), "feed");
        expect(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK, "synchronize");
    };
    const std::string prefix = matching ? "https://" : "other://";
    const auto uri = prefix + std::string(length - prefix.size(), 'a');
    feed("\x1b]8;;" + uri + "\x1b\\link\x1b]8;;\x1b\\");
    const auto rules = rules_for(scenario);
    const auto expected_command = scenario == "custom" ? "custom-action-2" : "xdg-open";
#ifdef BOXRIG_OBSERVE_OSC8
    const std::size_t repetitions = 8;
    const int observed = 1;
#else
    const std::size_t repetitions = length == 8192 ? 100 : 1000;
    const int observed = 0;
#endif
    for (int trial = 0; trial < 3; trial++) {
        const std::unique_ptr<TerminalLinks, decltype(&destroy_terminal_links)>
            links(create_terminal_links(view, rules), destroy_terminal_links);
        for (int phase = 0; phase < 2; phase++) {
            observation = {};
            const auto count = phase == 0 ? 1 : repetitions;
            std::chrono::nanoseconds resolve_time{};
            const auto begin = std::chrono::steady_clock::now();
            for (std::size_t index = 0; index < count; index++) {
                if (activity == "update")
                    feed(index % 2 == 0 ? "\x1b[2;1Ha" : "\x1b[2;1Hb");
                const auto column = activity == "move" ? index % 4 : 0;
                boxrig_ui_hit_test_result hit{};
                expect(boxrig_ui_view_hit_test(view,
                    static_cast<boxrig_ui_fixed>((1 + column * 8) * BOXRIG_UI_FIXED_ONE),
                    BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK &&
                    hit.hyperlink_uri_length == length, "current URI hit");
                const auto resolve_begin = std::chrono::steady_clock::now();
                const auto result = terminal_links_resolve(links.get(), hit);
                resolve_time += std::chrono::steady_clock::now() - resolve_begin;
                expect(static_cast<bool>(result && result->action) == matching, "resolved outcome");
                if (matching) {
                    expect(result->action->command == expected_command, "ordered rule action");
                    expect(result->action->arguments == std::vector<std::string>{uri}, "capture expansion");
                }
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count();
            std::printf("%s,%zu,%s,%s,%d,%s,%zu,%zu,%d,%.3f,%.3f,%zu,%zu,%zu,%zu\n",
                scenario.data(), length, matching ? "match" : "miss", activity.data(), trial,
                phase == 0 ? "cold" : "warm", rules.size(), count, observed,
                static_cast<double>(resolve_time.count()) / count,
                static_cast<double>(elapsed) / count, observation.matchers,
                observation.prepared_inputs, observation.prepared_bytes, observation.work);
        }
    }
}

int main(int argc, char **argv)
{
    try {
        if (argc != 2 || (std::string_view(argv[1]) != "default" && std::string_view(argv[1]) != "custom")) {
            std::fputs("usage: osc8_resolve_cost default|custom\n", stderr);
            return 2;
        }
        std::puts("scenario,uri_bytes,outcome,activity,trial,phase,rules,repetitions,observed,resolve_ns,driver_ns,matchers,prepared_inputs,prepared_bytes,work");
        for (const auto length : {std::size_t{64}, std::size_t{512}, std::size_t{8192}})
            for (const auto matching : {true, false})
                for (const auto activity : {"held", "move", "update"})
                    measure(argv[1], length, matching, activity);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
