#include "bandcampplugin.h"

#include "bandcampdialog.h"
#include "bandcampinput.h"

#include <core/plugins/coreplugincontext.h>
#include <core/player/playercontroller.h>
#include <core/playlist/playlisthandler.h>
#include <gui/guiconstants.h>
#include <gui/plugins/guiplugincontext.h>
#include <utils/actions/actioncontainer.h>
#include <utils/actions/actionmanager.h>

#include <QAction>

using namespace Qt::StringLiterals;

namespace Fooyin::Bandcamp {

void BandcampPlugin::initialise(const CorePluginContext& context)
{
    m_playlists = context.playlistHandler;
    m_player    = context.playerController;
}

void BandcampPlugin::initialise(const GuiPluginContext& context)
{
    m_actionManager = context.actionManager;

    // Add "Stream from Bandcamp…" to the Library menu bar entry.
    auto* libraryMenu = m_actionManager->actionContainer(Constants::Menus::Library);
    if (!libraryMenu)
        return;

    auto* action = new QAction(tr("Stream from Bandcamp\u2026"), this);
    m_actionManager->registerAction(action, "Bandcamp.OpenStreamDialog");
    libraryMenu->addAction(action);

    QObject::connect(action, &QAction::triggered, this, [this]() {
        showSettings(nullptr);
    });
}

void BandcampPlugin::shutdown()
{
    m_playlists     = nullptr;
    m_player        = nullptr;
    m_actionManager = nullptr;
}

// ── Plugin ────────────────────────────────────────────────────────────────

bool BandcampPlugin::hasSettings() const { return true; }

void BandcampPlugin::showSettings(QWidget* parent)
{
    auto* dlg = new BandcampStreamDialog(m_playlists, m_player, parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

// ── InputPlugin ───────────────────────────────────────────────────────────

QString BandcampPlugin::inputName() const
{
    return u"Bandcamp Input"_s;
}

InputCreator BandcampPlugin::inputCreator() const
{
    InputCreator c;
    c.decoder = []() { return std::make_unique<BandcampDecoder>(); };
    c.reader  = []() { return std::make_unique<BandcampReader>(); };
    return c;
}

} // namespace Fooyin::Bandcamp

#include "moc_bandcampplugin.cpp"
