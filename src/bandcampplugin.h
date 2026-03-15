#pragma once

#include <core/engine/inputplugin.h>
#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <gui/plugins/guiplugin.h>

#include <QObject>

namespace Fooyin {
class ActionManager;
class PlaylistHandler;
class PlayerController;
}

namespace Fooyin::Bandcamp {

class BandcampPlugin : public QObject,
                       public Plugin,
                       public CorePlugin,
                       public InputPlugin,
                       public GuiPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.fooyin.fooyin.plugin/1.0" FILE "bandcamp.json")
    Q_INTERFACES(Fooyin::Plugin Fooyin::CorePlugin Fooyin::InputPlugin Fooyin::GuiPlugin)

public:
    // CorePlugin
    void initialise(const CorePluginContext& context) override;

    // GuiPlugin — called after CorePlugin; adds the Library menu entry
    void initialise(const GuiPluginContext& context) override;

    // Plugin
    void shutdown() override;
    [[nodiscard]] bool hasSettings() const override;
    void showSettings(QWidget* parent) override;

    // InputPlugin
    [[nodiscard]] QString      inputName()    const override;
    [[nodiscard]] InputCreator inputCreator() const override;

private:
    PlaylistHandler*  m_playlists{nullptr};
    PlayerController* m_player{nullptr};
    ActionManager*    m_actionManager{nullptr};
};

} // namespace Fooyin::Bandcamp
