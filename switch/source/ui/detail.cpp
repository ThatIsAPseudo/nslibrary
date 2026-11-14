#include "ui/detail.hpp"

#include "app/services.hpp"
#include "app/session.hpp"
#include "install/preflight.hpp"
#include "installed/compare.hpp"
#include "ui/format.hpp"
#include "ui/icon_cache.hpp"
#include "ui/main_activity.hpp"
#include "ui/widgets.hpp"

#include <atomic>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace brls::literals;

namespace nslib {
namespace {

constexpr float kArt = 160;

/** Title ids come from two sources; only their hex digits are meaningful, not their case. */
std::string upperHex(std::string id) {
    for (char& c : id) {
        if (c >= 'a' && c <= 'f') c = char(c - 'a' + 'A');
    }
    return id;
}

/** What this console already has of the title, in one line. */
std::string installedLine(const InstalledSummary& s) {
    if (!s.baseInstalled && s.patchVersion == 0) return "app/detail/not_installed"_i18n;
    std::string line = "app/detail/installed"_i18n;
    if (s.baseInstalled && !s.baseStorage.empty()) line += " (" + tr(storageKey(s.baseStorage), s.baseStorage) + ")";
    if (s.patchVersion) line += "   " + "app/type/patch"_i18n + " " + formatVersion(s.patchVersion);
    return line;
}

/** Title ids of everything on this console, so each row can say whether it is already there. */
std::unordered_set<std::string> installedIds() {
    std::unordered_set<std::string> ids;
    for (const auto& t : Session::instance().installedSnapshot().titles) ids.insert(upperHex(t.titleId));
    return ids;
}

std::string withInstalledMark(const std::string& text, bool installed) {
    if (!installed) return text;
    return text + "   " + "app/detail/on_console"_i18n;
}

/** "v65536   Released 2018-11-20   12.4 GB": what the server knows about one content. */
std::string contentLine(const CatalogApp& app, int64_t contentMetaId, uint32_t version, uint64_t size) {
    std::string line;
    if (version) line = formatVersion(version);
    const auto date = app.releaseDates.find(contentMetaId);
    if (date != app.releaseDates.end()) {
        const std::string released = formatReleaseDate(date->second);
        if (!released.empty()) {
            if (!line.empty()) line += "   ";
            line += brls::getStr("app/detail/released", released);
        }
    }
    if (!line.empty()) line += "   ";
    return line + formatSize(size);
}

std::string expectedComponentLine(const CatalogComponent& component) {
    std::string line;
    if (component.version && *component.version) line = formatVersion(*component.version);
    if (component.releaseDate) {
        const std::string released = formatReleaseDate(*component.releaseDate);
        if (!released.empty()) {
            if (!line.empty()) line += "   ";
            line += brls::getStr("app/detail/released", released);
        }
    }
    if (!line.empty()) line += "   ";
    line += component.size ? formatSize(*component.size) : "app/detail/size_unknown"_i18n;
    return line;
}

std::string componentTitle(const CatalogComponent& component, const std::string& appName) {
    if (component.type == "patch") {
        std::string label = "app/type/patch"_i18n;
        if (component.version) label += " " + formatVersion(*component.version);
        return label;
    }
    if (component.type == "addon") {
        return cleanTitleName(component.name.empty() ? component.titleId : component.name);
    }
    return cleanTitleName(component.name.empty() ? appName : component.name);
}

std::vector<const CatalogComponent*> componentsOf(const CatalogApp& app, const std::string& type) {
    std::vector<const CatalogComponent*> components;
    for (const auto& component : app.components) {
        if (component.type == type) components.push_back(&component);
    }
    return components;
}

brls::Label* makeInfoLabel(const std::string& text, float size, const char* color) {
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(size);
    label->setSingleLine(true);
    // A label is laid out at its font size, so stacked lines need their own breathing room.
    label->setMarginBottom(8);
    if (color) label->setTextColor(brls::Application::getTheme()[color]);
    return label;
}

} // namespace

void confirmInstall(int64_t contentMetaId, const std::string& name, uint64_t size,
    std::optional<uint32_t> requiredSystemVersion)
{
    if (!contentMetaId) {
        showError("app/library/nothing"_i18n);
        return;
    }
    std::string body = "app/detail/install_body"_i18n + std::string("\n") + cleanTitleName(name);
    if (size) body += "\n" + formatSize(size);
    if (requiredSystemVersion && firmwareTooNew(*requiredSystemVersion, currentFirmwarePacked())) {
        const bool clearing = Session::instance().settings.clearFirmwareRequirement;
        body += "\n\n" + brls::getStr(clearing ? "app/detail/clear_firmware" : "app/detail/warn_firmware_version",
                               formatFirmware(*requiredSystemVersion));
    }
    if (batteryShouldWarn(batteryPercent(), batteryCharging())) {
        body += "\n\n" + "app/detail/warn_battery"_i18n;
    }
    auto* dialog = new brls::Dialog(body);
    auto enqueue = [contentMetaId](const std::string& target) {
        return [contentMetaId, target]() {
            // Start after the dialog has closed so the progress overlay does not draw over it.
            brls::sync([contentMetaId, target] { Session::instance().queueInstall(contentMetaId, target); });
        };
    };
    // The storage picked in Settings goes first, so the highlighted button is the usual answer.
    std::vector<std::string> targets = {"sd", "nand", "auto"};
    const std::string preferred = Session::instance().settings.defaultTarget;
    for (size_t i = 0; i < targets.size(); i++) {
        if (targets[i] != preferred) continue;
        std::swap(targets[0], targets[i]);
        break;
    }
    for (const auto& target : targets) dialog->addButton(tr(storageKey(target), target), enqueue(target));
    dialog->open();
}

TitleDetailActivity::TitleDetailActivity(CatalogApp app) : app_(std::move(app)) {}

TitleDetailActivity::~TitleDetailActivity() { alive_->store(false); }

brls::View* TitleDetailActivity::createContentView() {
    const std::string name = cleanTitleName(app_.name.empty() ? app_.id : app_.name);
    auto* scroll = new brls::ScrollingFrame();
    auto* box = new brls::Box(brls::Axis::COLUMN);
    box->setPadding(24, 40, 30, 40);
    scroll->setContentView(box);
    auto* frame = new brls::AppletFrame(scroll);
    frame->setTitle(name);

    auto* header = new brls::Box(brls::Axis::ROW);
    header->setAlignItems(brls::AlignItems::CENTER);
    header->setMarginBottom(8);

    auto* art = new brls::Box();
    art->setWidth(kArt);
    art->setHeight(kArt);
    art->setCornerRadius(8);
    art->setBackgroundColor(brls::Application::getTheme()["brls/sidebar/background"]);
    auto* icon = new brls::Image();
    icon->setWidth(kArt);
    icon->setHeight(kArt);
    icon->setCornerRadius(8);
    icon->setScalingType(brls::ImageScalingType::FILL);
    art->addView(icon);
    header->addView(art);
    loadAppIcon(icon, alive_, app_.id, app_.iconRev);

    // The applet frame already shows the name, so this column carries only what it does not.
    auto* info = new brls::Box(brls::Axis::COLUMN);
    info->setPaddingLeft(24);
    info->setGrow(1.0f);
    if (!app_.publisher.empty()) info->addView(makeInfoLabel(cleanTitleName(app_.publisher), 20, nullptr));
    const auto summary = summarizeInstalled(Session::instance().installedSnapshot().titles, app_.id);
    info->addView(makeInfoLabel(installedLine(summary), 18,
        summary.baseInstalled ? "brls/accent" : "brls/text_disabled"));
    if (app_.requiredSysVersion) {
        info->addView(makeInfoLabel(
            "app/detail/firmware"_i18n + std::string(" ") + formatFirmware(*app_.requiredSysVersion), 18,
            "brls/text_disabled"));
    }
    info->addView(makeInfoLabel(upperHex(app_.id), 16, "brls/text_disabled"));
    header->addView(info);
    box->addView(header);

    const auto installed = installedIds();

    if (!app_.components.empty()) {
        const std::pair<std::string, std::string> sections[] = {
            {"application", "app/detail/base"_i18n},
            {"patch", "app/type/patches"_i18n},
            {"addon", "app/type/addons"_i18n},
        };
        for (const auto& section : sections) {
            const auto components = componentsOf(app_, section.first);
            if (components.empty()) continue;
            box->addView(makeHeader(section.second, std::to_string(components.size())));
            for (const auto* component : components) {
                const std::string label = componentTitle(*component, name);
                if (!component->inLibrary || !component->contentMetaId) {
                    box->addView(makeInfoCell(
                        label + "   " + "app/detail/missing_library"_i18n,
                        expectedComponentLine(*component)));
                    continue;
                }

                bool onConsole = false;
                if (component->type == "application") {
                    onConsole = summary.baseInstalled;
                } else if (component->type == "patch") {
                    onConsole = component->version && *component->version &&
                        summary.patchVersion >= *component->version;
                } else {
                    onConsole = installed.count(upperHex(component->titleId)) > 0;
                }
                const int64_t contentMetaId = *component->contentMetaId;
                const uint64_t size = component->size.value_or(0);
                const std::string installName = component->type == "patch"
                    ? name + " " + (component->version ? formatVersion(*component->version) : "")
                    : label;
                box->addView(makeCell(withInstalledMark(label, onConsole), expectedComponentLine(*component),
                    [app = app_, contentMetaId, size, installName](brls::View*) {
                        confirmInstall(contentMetaId, installName, size, app.requiredSysVersion);
                        return true;
                    }));
            }
        }
    } else {
        // Older servers send no component inventory; preserve their library-only detail view.
        if (app_.base) {
            const auto base = *app_.base;
            box->addView(makeHeader("app/detail/base"_i18n));
            box->addView(makeCell(withInstalledMark(name, summary.baseInstalled),
                contentLine(app_, base.contentMetaId, base.version, base.size),
                [app = app_, base, name](brls::View*) {
                    confirmInstall(base.contentMetaId, name, base.size, app.requiredSysVersion);
                    return true;
                }));
        }

        if (!app_.updates.empty()) {
            box->addView(makeHeader("app/type/patches"_i18n, std::to_string(app_.updates.size())));
            for (const auto& upd : app_.updates) {
                const std::string label = "app/type/patch"_i18n + std::string(" ") + formatVersion(upd.version);
                box->addView(makeCell(withInstalledMark(label, summary.patchVersion >= upd.version && upd.version),
                    contentLine(app_, upd.contentMetaId, upd.version, upd.size),
                    [app = app_, upd, name](brls::View*) {
                        confirmInstall(upd.contentMetaId, name + " " + formatVersion(upd.version), upd.size,
                            app.requiredSysVersion);
                        return true;
                    }));
            }
        }

        if (!app_.dlc.empty()) {
            box->addView(makeHeader("app/type/addons"_i18n, std::to_string(app_.dlc.size())));
            for (const auto& dlc : app_.dlc) {
                const std::string label = dlc.name.empty() ? upperHex(dlc.titleId) : cleanTitleName(dlc.name);
                box->addView(makeCell(withInstalledMark(label, installed.count(upperHex(dlc.titleId)) > 0),
                    contentLine(app_, dlc.contentMetaId, dlc.version, dlc.size),
                    [dlc, label](brls::View*) {
                        confirmInstall(dlc.contentMetaId, label, dlc.size);
                        return true;
                    }));
            }
        }

        if (!app_.base && app_.updates.empty() && app_.dlc.empty()) {
            box->addView(makeEmptyState("app/library/nothing"_i18n));
        }
    }
    return frame;
}

} // namespace nslib
