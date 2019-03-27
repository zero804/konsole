/*
    Copyright 2006-2008 by Robert Knight <robertknight@gmail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA.
*/

// Own
#include "ViewManager.h"

#include <config-konsole.h>

// Qt
#include <QStringList>
#include <QTabBar>

// KDE
#include <KAcceleratorManager>
#include <KLocalizedString>
#include <KActionCollection>
#include <KConfigGroup>

// Konsole
#include <windowadaptor.h>

#include "ColorScheme.h"
#include "ColorSchemeManager.h"
#include "Session.h"
#include "TerminalDisplay.h"
#include "SessionController.h"
#include "SessionManager.h"
#include "ProfileManager.h"
#include "ViewSplitter.h"
#include "Enumeration.h"
#include "ViewContainer.h"

using namespace Konsole;

int ViewManager::lastManagerId = 0;

ViewManager::ViewManager(QObject *parent, KActionCollection *collection) :
    QObject(parent),
    _viewContainer(nullptr),
    _pluggedController(nullptr),
    _sessionMap(QHash<TerminalDisplay *, Session *>()),
    _actionCollection(collection),
    _navigationMethod(NoNavigation),
    _navigationVisibility(NavigationNotSet),
    _newTabBehavior(PutNewTabAtTheEnd),
    _managerId(0),
    _terminalDisplayHistoryIndex(-1)
{
    _viewContainer = createContainer();
    // setup actions which are related to the views
    setupActions();

    /* TODO: Reconnect
    // emit a signal when all of the views held by this view manager are destroyed
    */
    connect(_viewContainer.data(), &Konsole::TabbedViewContainer::empty,
            this, &Konsole::ViewManager::empty);

    // listen for profile changes
    connect(ProfileManager::instance(), &Konsole::ProfileManager::profileChanged,
            this, &Konsole::ViewManager::profileChanged);
    connect(SessionManager::instance(), &Konsole::SessionManager::sessionUpdated,
            this, &Konsole::ViewManager::updateViewsForSession);

    //prepare DBus communication
    new WindowAdaptor(this);

    _managerId = ++lastManagerId;
    QDBusConnection::sessionBus().registerObject(QLatin1String("/Windows/")
                                                 + QString::number(_managerId), this);
}

ViewManager::~ViewManager() = default;

int ViewManager::managerId() const
{
    return _managerId;
}

QWidget *ViewManager::activeView() const
{
    return _viewContainer->currentWidget();
}

QWidget *ViewManager::widget() const
{
    return _viewContainer;
}

void ViewManager::setupActions()
{
    Q_ASSERT(_actionCollection);
    if (_actionCollection == nullptr) {
        return;
    }

    KActionCollection *collection = _actionCollection;

    // Let's reuse the pointer, no need not to.
    auto *action = new QAction();
    action->setIcon(QIcon::fromTheme(QStringLiteral("view-split-left-right")));
    action->setText(i18nc("@action:inmenu", "Split View Left/Right"));
    connect(action, &QAction::triggered, this, &ViewManager::splitLeftRight);
    collection->addAction(QStringLiteral("split-view-left-right"), action);
    collection->setDefaultShortcut(action, Konsole::ACCEL + Qt::Key_ParenLeft);

    action = new QAction();
    action->setIcon(QIcon::fromTheme(QStringLiteral("view-split-top-bottom")));
    action->setText(i18nc("@action:inmenu", "Split View Top/Bottom"));
    connect(action, &QAction::triggered, this, &ViewManager::splitTopBottom);
    collection->setDefaultShortcut(action, Konsole::ACCEL + Qt::Key_ParenRight);
    collection->addAction(QStringLiteral("split-view-top-bottom"), action);

    action = new QAction();
    action->setText(i18nc("@action:inmenu", "Expand View"));
    action->setEnabled(false);
    connect(action, &QAction::triggered, this, &ViewManager::expandActiveContainer);
    collection->setDefaultShortcut(action, Konsole::ACCEL + Qt::SHIFT + Qt::Key_BracketRight);
    collection->addAction(QStringLiteral("expand-active-view"), action);
    _multiSplitterOnlyActions << action;

    action = new QAction();
    action->setText(i18nc("@action:inmenu", "Shrink View"));
    collection->setDefaultShortcut(action, Konsole::ACCEL + Qt::SHIFT + Qt::Key_BracketLeft);
    action->setEnabled(false);
    collection->addAction(QStringLiteral("shrink-active-view"), action);
    connect(action, &QAction::triggered, this, &ViewManager::shrinkActiveContainer);
    _multiSplitterOnlyActions << action;

    // Crashes on Mac.
#if defined(ENABLE_DETACHING)
    action = collection->addAction(QStringLiteral("detach-view"));
    action->setEnabled(true);
    action->setIcon(QIcon::fromTheme(QStringLiteral("tab-detach")));
    action->setText(i18nc("@action:inmenu", "Detach Current &View"));

    connect(action, &QAction::triggered, this, &ViewManager::detachActiveView);
    _multiSplitterOnlyActions << action;

    // Ctrl+Shift+D is not used as a shortcut by default because it is too close
    // to Ctrl+D - which will terminate the session in many cases
    collection->setDefaultShortcut(action, Konsole::ACCEL + Qt::SHIFT + Qt::Key_H);

    action = collection->addAction(QStringLiteral("detach-tab"));
    action->setEnabled(true);
    action->setIcon(QIcon::fromTheme(QStringLiteral("tab-detach")));
    action->setText(i18nc("@action:inmenu", "Detach Current &Tab"));
    connect(action, &QAction::triggered, this, &ViewManager::detachActiveTab);
    _multiTabOnlyActions << action;
    // Ctrl+Shift+D is not used as a shortcut by default because it is too close
    // to Ctrl+D - which will terminate the session in many cases
    collection->setDefaultShortcut(action, Konsole::ACCEL + Qt::SHIFT + Qt::Key_L);
#endif

    // keyboard shortcut only actions
    action = new QAction(i18nc("@action Shortcut entry", "Next Tab"), this);
    const QList<QKeySequence> nextViewActionKeys{Qt::SHIFT + Qt::Key_Right, Qt::CTRL + Qt::Key_PageDown};
    collection->setDefaultShortcuts(action, nextViewActionKeys);
    collection->addAction(QStringLiteral("next-tab"), action);
    connect(action, &QAction::triggered, this, &ViewManager::nextView);
    _multiTabOnlyActions << action;
    // _viewSplitter->addAction(nextViewAction);

    action = new QAction(i18nc("@action Shortcut entry", "Previous Tab"), this);
    const QList<QKeySequence> previousViewActionKeys{Qt::SHIFT + Qt::Key_Left, Qt::CTRL + Qt::Key_PageUp};
    collection->setDefaultShortcuts(action, previousViewActionKeys);
    collection->addAction(QStringLiteral("previous-tab"), action);
    connect(action, &QAction::triggered, this, &ViewManager::previousView);
    _multiTabOnlyActions << action;
    // _viewSplitter->addAction(previousViewAction);

    action = new QAction(i18nc("@action Shortcut entry", "Next View Container"), this);
    connect(action, &QAction::triggered, this, &ViewManager::focusUp);
    collection->addAction(QStringLiteral("next-container"), action);
    collection->setDefaultShortcut(action, Qt::SHIFT + Qt::CTRL + Qt::Key_Up);
    _viewContainer->addAction(action);
    _multiSplitterOnlyActions << action;

    action = new QAction(QStringLiteral("Focus Down"));
    collection->setDefaultShortcut(action, Qt::SHIFT + Qt::CTRL + Qt::Key_Down);
    connect(action, &QAction::triggered, this, &ViewManager::focusDown);
    _multiSplitterOnlyActions << action;
    _viewContainer->addAction(action);

    action = new QAction(i18nc("@action Shortcut entry", "Move Tab Left"), this);
    collection->setDefaultShortcut(action, Konsole::ACCEL + Qt::SHIFT + Konsole::LEFT);
    connect(action, &QAction::triggered, this, &ViewManager::focusLeft);
    collection->addAction(QStringLiteral("move-view-left"), action);
    _multiSplitterOnlyActions << action;

    action = new QAction(i18nc("@action Shortcut entry", "Move Tab Right"), this);
    collection->setDefaultShortcut(action, Konsole::ACCEL + Qt::SHIFT + Konsole::RIGHT);
    connect(action, &QAction::triggered, this, &ViewManager::focusRight);
    collection->addAction(QStringLiteral("move-view-right"), action);
    _multiSplitterOnlyActions << action;

    action = new QAction(i18nc("@action Shortcut entry", "Switch to Last Tab"), this);
    connect(action, &QAction::triggered, this, &ViewManager::lastView);
    collection->addAction(QStringLiteral("last-tab"), action);
    _multiTabOnlyActions << action;

    action = new QAction(i18nc("@action Shortcut entry", "Last Used Tabs"), this);
    connect(action, &QAction::triggered, this, &ViewManager::lastUsedView);
    collection->setDefaultShortcut(action, Qt::CTRL + Qt::Key_Tab);
    collection->addAction(QStringLiteral("last-used-tab"), action);
    _multiTabOnlyActions << action;

    action = new QAction(i18nc("@action Shortcut entry", "Toggle Between Two Tabs"), this);
    connect(action, &QAction::triggered, this, &Konsole::ViewManager::toggleTwoViews);
    collection->addAction(QStringLiteral("toggle-two-tabs"), action);
    _multiTabOnlyActions << action;

    action = new QAction(i18nc("@action Shortcut entry", "Last Used Tabs (Reverse)"), this);
    collection->addAction(QStringLiteral("last-used-tab-reverse"), action);
    collection->setDefaultShortcut(action, Qt::CTRL + Qt::SHIFT + Qt::Key_Tab);
    connect(action, &QAction::triggered, this, &ViewManager::lastUsedViewReverse);
    _multiTabOnlyActions << action;

    action = new QAction(i18nc("@action Shortcut entry", "Maximize current Terminal"), this);
    collection->addAction(QStringLiteral("maximize-current-terminal"), action);
    collection->setDefaultShortcut(action, Qt::CTRL + Qt::SHIFT + Qt::Key_E);
    connect(action, &QAction::triggered, _viewContainer, &TabbedViewContainer::maximizeCurrentTerminal);
    _multiSplitterOnlyActions << action;
    _viewContainer->addAction(action);

    action = new QAction(i18nc("@action Shortcut entry", "Restore other terminals"), this);
    collection->addAction(QStringLiteral("restore-other-terminals"), action);
    collection->setDefaultShortcut(action, Qt::CTRL + Qt::SHIFT + Qt::Key_Minus);
    connect(action, &QAction::triggered, _viewContainer, &TabbedViewContainer::restoreOtherTerminals);
    _multiSplitterOnlyActions << action;
    _viewContainer->addAction(action);

    // _viewSplitter->addAction(lastUsedViewReverseAction);
    const int SWITCH_TO_TAB_COUNT = 19;
    for (int i = 0; i < SWITCH_TO_TAB_COUNT; i++) {
        action = new QAction(i18nc("@action Shortcut entry", "Switch to Tab %1", i + 1), this);
        connect(action, &QAction::triggered, this, [this, i]() { switchToView(i); });
        collection->addAction(QStringLiteral("switch-to-tab-%1").arg(i), action);
    }

    auto handleMultiTabActionsLambda = [=]{
        const int count = _viewContainer->count();
        foreach(QAction *action, _multiTabOnlyActions) {
            action->setEnabled(count > 1);
        }
    };
    connect(_viewContainer, &TabbedViewContainer::viewAdded, this, handleMultiTabActionsLambda);
    connect(_viewContainer, &TabbedViewContainer::viewRemoved, this, handleMultiTabActionsLambda);

    connect(_viewContainer, &QTabWidget::currentChanged, this, &ViewManager::updateDetachViewState);

    // Initial state
    handleMultiTabActionsLambda();
    updateDetachViewState();
}

void ViewManager::switchToView(int index)
{
    _viewContainer->setCurrentIndex(index);
}

void ViewManager::switchToTerminalDisplay(Konsole::TerminalDisplay* terminalDisplay)
{
    auto splitter = qobject_cast<ViewSplitter*>(terminalDisplay->parentWidget());
    auto toplevelSplitter = splitter->getToplevelSplitter();

    // Focus the TermialDisplay
    terminalDisplay->setFocus();

    if (_viewContainer->currentWidget() != toplevelSplitter) {
        // Focus the tab
        switchToView(_viewContainer->indexOf(toplevelSplitter));
    }
}

void ViewManager::updateDetachViewState()
{
    if (_viewContainer && _viewContainer->activeViewSplitter()) {
        const int splitCount = _viewContainer
                ->activeViewSplitter()
                ->getToplevelSplitter()
                ->findChildren<TerminalDisplay*>()
                    .count();

        foreach (QAction *action, _multiSplitterOnlyActions) {
            action->setEnabled(splitCount > 1);
        }
    }
}

void ViewManager::focusUp()
{
    _viewContainer->activeViewSplitter()->focusUp();
}

void ViewManager::focusDown()
{
    _viewContainer->activeViewSplitter()->focusDown();
}

void ViewManager::focusLeft()
{
    _viewContainer->activeViewSplitter()->focusLeft();
}

void ViewManager::focusRight()
{
    _viewContainer->activeViewSplitter()->focusRight();
}

void ViewManager::moveActiveViewLeft()
{
    _viewContainer->moveActiveView(TabbedViewContainer::MoveViewLeft);
}

void ViewManager::moveActiveViewRight()
{
    _viewContainer->moveActiveView(TabbedViewContainer::MoveViewRight);
}

void ViewManager::nextContainer()
{
//    _viewSplitter->activateNextContainer();
}

void ViewManager::nextView()
{
    _viewContainer->activateNextView();
}

void ViewManager::previousView()
{
    _viewContainer->activatePreviousView();
}

void ViewManager::lastView()
{
    _viewContainer->activateLastView();
}

void ViewManager::activateLastUsedView(bool reverse)
{
    if (_terminalDisplayHistory.count() <= 1) {
        return;
    }

    if (_terminalDisplayHistoryIndex == -1) {
        _terminalDisplayHistoryIndex = reverse ? _terminalDisplayHistory.count() - 1 : 1;
    } else if (reverse) {
        if (_terminalDisplayHistoryIndex == 0) {
            _terminalDisplayHistoryIndex = _terminalDisplayHistory.count() - 1;
        } else {
            _terminalDisplayHistoryIndex--;
        }
    } else {
        if (_terminalDisplayHistoryIndex >= _terminalDisplayHistory.count() - 1) {
            _terminalDisplayHistoryIndex = 0;
        } else {
            _terminalDisplayHistoryIndex++;
        }
    }

    switchToTerminalDisplay(_terminalDisplayHistory[_terminalDisplayHistoryIndex]);
}

void ViewManager::lastUsedView()
{
    activateLastUsedView(false);
}

void ViewManager::lastUsedViewReverse()
{
    activateLastUsedView(true);
}

void ViewManager::toggleTwoViews()
{
    if (_terminalDisplayHistory.count() <= 1) {
        return;
    }

    switchToTerminalDisplay(_terminalDisplayHistory.at(1));
}

void ViewManager::detachActiveView()
{
#if !defined(ENABLE_DETACHING)
    return;
#endif
    // find the currently active view and remove it from its container
    if ((_viewContainer->findChildren<TerminalDisplay*>()).count() > 1) {
        auto activeSplitter = _viewContainer->activeViewSplitter();
        auto terminal = activeSplitter->activeTerminalDisplay();
        auto newSplitter = new ViewSplitter();
        newSplitter->addTerminalDisplay(terminal, Qt::Horizontal);
        QHash<TerminalDisplay*, Session*> detachedSessions = forgetAll(newSplitter);
        emit terminalsDetached(newSplitter, detachedSessions);
        focusAnotherTerminal(activeSplitter->getToplevelSplitter());
        updateDetachViewState();
    }
}

void ViewManager::detachActiveTab()
{
    const int currentIdx = _viewContainer->currentIndex();
    detachTab(currentIdx);
}

void ViewManager::detachTab(int tabIdx)
{
#if !defined(ENABLE_DETACHING)
    return;
#endif
    ViewSplitter* splitter = _viewContainer->viewSplitterAt(tabIdx);
    QHash<TerminalDisplay*, Session*> detachedSessions = forgetAll(_viewContainer->viewSplitterAt(tabIdx));
    emit terminalsDetached(splitter, detachedSessions);
}

QHash<TerminalDisplay*, Session*> ViewManager::forgetAll(ViewSplitter* splitter) {
    splitter->setParent(nullptr);
    QHash<TerminalDisplay*, Session*> detachedSessions;
    foreach(TerminalDisplay* terminal, splitter->findChildren<TerminalDisplay*>()) {
        Session* session = forgetTerminal(terminal);
        detachedSessions[terminal] = session;
    }
    return detachedSessions;
}

Session* ViewManager::forgetTerminal(TerminalDisplay* terminal)
{
    removeController(terminal->sessionController());
    auto session = _sessionMap.take(terminal);
    if (session != nullptr) {
        disconnect(session, &Konsole::Session::finished, this, &Konsole::ViewManager::sessionFinished);
    }
    _viewContainer->disconnectTerminalDisplay(terminal);
    updateTerminalDisplayHistory(terminal, true);
    return session;
}

void ViewManager::sessionFinished()
{
    // if this slot is called after the view manager's main widget
    // has been destroyed, do nothing
    if (_viewContainer.isNull()) {
        return;
    }

    auto *session = qobject_cast<Session *>(sender());
    Q_ASSERT(session);

    auto view = _sessionMap.key(session);
    _sessionMap.remove(view);

    // Before deleting the view, let's unmaximize if it's maximized.
    auto splitter = qobject_cast<ViewSplitter*>(view->parentWidget());
    auto toplevelSplitter = splitter->getToplevelSplitter();
    toplevelSplitter->restoreOtherTerminals();
    _viewContainer->removeView(view);
    view->deleteLater();

    // Only remove the controller from factory() if it's actually controlling
    // the session from the sender.
    // This fixes BUG: 348478 - messed up menus after a detached tab is closed
    if ((!_pluggedController.isNull()) && (_pluggedController->session() == session)) {
        // This is needed to remove this controller from factory() in
        // order to prevent BUG: 185466 - disappearing menu popup
        emit unplugController(_pluggedController);
    }

    updateTerminalDisplayHistory(view, true);
    focusAnotherTerminal(toplevelSplitter);
    updateDetachViewState();
}

void ViewManager::focusAnotherTerminal(ViewSplitter *toplevelSplitter)
{
    auto tabTterminalDisplays = toplevelSplitter->findChildren<TerminalDisplay*>();
    if (tabTterminalDisplays.count() > 1) {
        // Give focus to the last used terminal in this tab
        for (auto *historyItem : _terminalDisplayHistory) {
            for (auto *terminalDisplay : tabTterminalDisplays) {
                if (terminalDisplay == historyItem) {
                    terminalDisplay->setFocus(Qt::OtherFocusReason);
                    return;
                }
            }
        }
    } else if (_terminalDisplayHistory.count() >= 1) {
        // Give focus to the last used terminal tab
        switchToTerminalDisplay(_terminalDisplayHistory[0]);
    }
}

void ViewManager::viewActivated(TerminalDisplay *view)
{
    Q_ASSERT(view != nullptr);

    // focus the activated view, this will cause the SessionController
    // to notify the world that the view has been focused and the appropriate UI
    // actions will be plugged in.
    view->setFocus(Qt::OtherFocusReason);
}

void ViewManager::splitLeftRight()
{
    splitView(Qt::Horizontal);
}

void ViewManager::splitTopBottom()
{
    splitView(Qt::Vertical);
}

void ViewManager::splitView(Qt::Orientation orientation)
{
    auto viewSplitter = qobject_cast<ViewSplitter*>(_viewContainer->currentWidget());

    // get the currently applied profile and use it to create the new tab.
    auto *currentDisplay = viewSplitter->findChild<TerminalDisplay*>();
    auto profile = SessionManager::instance()->sessionProfile(_sessionMap[currentDisplay]);

    // Create a new session with the selected profile.
    auto *session = SessionManager::instance()->createSession(profile);
    session->addEnvironmentEntry(QStringLiteral("KONSOLE_DBUS_WINDOW=/Windows/%1").arg(managerId()));

    auto terminalDisplay = createView(session);

    _viewContainer->splitView(terminalDisplay, orientation);

    updateDetachViewState();

    // focus the new container
    terminalDisplay->setFocus();
}

void ViewManager::expandActiveContainer()
{
    _viewContainer->activeViewSplitter()->adjustActiveTerminalDisplaySize(10);
}

void ViewManager::shrinkActiveContainer()
{
    _viewContainer->activeViewSplitter()->adjustActiveTerminalDisplaySize(-10);
}

SessionController *ViewManager::createController(Session *session, TerminalDisplay *view)
{
    // create a new controller for the session, and ensure that this view manager
    // is notified when the view gains the focus
    auto controller = new SessionController(session, view, this);
    connect(controller, &Konsole::SessionController::focused, this,
            &Konsole::ViewManager::controllerChanged);
    connect(session, &Konsole::Session::destroyed, controller,
            &Konsole::SessionController::deleteLater);
    connect(session, &Konsole::Session::primaryScreenInUse, controller,
            &Konsole::SessionController::setupPrimaryScreenSpecificActions);
    connect(session, &Konsole::Session::selectionChanged, controller,
            &Konsole::SessionController::selectionChanged);
    connect(view, &Konsole::TerminalDisplay::destroyed, controller,
            &Konsole::SessionController::deleteLater);

    // if this is the first controller created then set it as the active controller
    if (_pluggedController.isNull()) {
        controllerChanged(controller);
    }

    return controller;
}

// should this be handed by ViewManager::unplugController signal
void ViewManager::removeController(SessionController* controller)
{
    disconnect(controller, &Konsole::SessionController::focused, this,
            &Konsole::ViewManager::controllerChanged);
    if (_pluggedController == controller) {
        _pluggedController = nullptr;
    }
    controller->deleteLater();
}

void ViewManager::controllerChanged(SessionController *controller)
{
    if (controller == _pluggedController) {
        return;
    }

    updateTerminalDisplayHistory(controller->view());

    _pluggedController = controller;
    emit activeViewChanged(controller);
}

SessionController *ViewManager::activeViewController() const
{
    return _pluggedController;
}

void ViewManager::attachView(TerminalDisplay *terminal, Session *session)
{
    connect(session, &Konsole::Session::finished, this, &Konsole::ViewManager::sessionFinished,
            Qt::UniqueConnection);
    _sessionMap[terminal] = session;
    createController(session, terminal);
    updateDetachViewState();
    _terminalDisplayHistory.append(terminal);
}

TerminalDisplay *ViewManager::createView(Session *session)
{
    // notify this view manager when the session finishes so that its view
    // can be deleted
    //
    // Use Qt::UniqueConnection to avoid duplicate connection
    connect(session, &Konsole::Session::finished, this, &Konsole::ViewManager::sessionFinished,
            Qt::UniqueConnection);
    TerminalDisplay *display = createTerminalDisplay(session);

    const Profile::Ptr profile = SessionManager::instance()->sessionProfile(session);
    applyProfileToView(display, profile);

    // set initial size
    const QSize &preferredSize = session->preferredSize();

    display->setSize(preferredSize.width(), preferredSize.height());
    createController(session, display);

    _sessionMap[display] = session;
    session->addView(display);
    _terminalDisplayHistory.append(display);

    // tell the session whether it has a light or dark background
    session->setDarkBackground(colorSchemeForProfile(profile)->hasDarkBackground());
    display->setFocus(Qt::OtherFocusReason);
//     updateDetachViewState();

    return display;
}

TabbedViewContainer *ViewManager::createContainer()
{
    auto *container = new TabbedViewContainer(this, nullptr);
    container->setNavigationVisibility(_navigationVisibility);
    connect(container, &TabbedViewContainer::detachTab, this, &ViewManager::detachTab);

    // connect signals and slots
    connect(container, &Konsole::TabbedViewContainer::viewAdded, this,
           [this, container]() {
               containerViewsChanged(container);
           });

    connect(container, &Konsole::TabbedViewContainer::viewRemoved, this,
           [this, container]() {
               containerViewsChanged(container);
           });

    connect(container, &TabbedViewContainer::newViewRequest,
            this, &ViewManager::newViewRequest);
    connect(container, &Konsole::TabbedViewContainer::newViewWithProfileRequest,
            this, &Konsole::ViewManager::newViewWithProfileRequest);
    connect(container, &Konsole::TabbedViewContainer::viewRemoved, this,
            &Konsole::ViewManager::viewDestroyed);
    connect(container, &Konsole::TabbedViewContainer::activeViewChanged, this,
            &Konsole::ViewManager::viewActivated);

    return container;
}

void ViewManager::setNavigationMethod(NavigationMethod method)
{
    Q_ASSERT(_actionCollection);
    if (_actionCollection == nullptr) {
        return;
    }
    KActionCollection *collection = _actionCollection;

    _navigationMethod = method;

    // FIXME: The following disables certain actions for the KPart that it
    // doesn't actually have a use for, to avoid polluting the action/shortcut
    // namespace of an application using the KPart (otherwise, a shortcut may
    // be in use twice, and the user gets to see an "ambiguous shortcut over-
    // load" error dialog). However, this approach sucks - it's the inverse of
    // what it should be. Rather than disabling actions not used by the KPart,
    // a method should be devised to only enable those that are used, perhaps
    // by using a separate action collection.

    const bool enable = (method != NoNavigation);

    auto enableAction = [&enable, &collection](const QString& actionName) {
        auto *action = collection->action(actionName);
        if (action != nullptr) {
            action->setEnabled(enable);
        }
    };

    enableAction(QStringLiteral("next-view"));
    enableAction(QStringLiteral("previous-view"));
    enableAction(QStringLiteral("last-tab"));
    enableAction(QStringLiteral("last-used-tab"));
    enableAction(QStringLiteral("last-used-tab-reverse"));
    enableAction(QStringLiteral("split-view-left-right"));
    enableAction(QStringLiteral("split-view-top-bottom"));
    enableAction(QStringLiteral("rename-session"));
    enableAction(QStringLiteral("move-view-left"));
    enableAction(QStringLiteral("move-view-right"));
}

ViewManager::NavigationMethod ViewManager::navigationMethod() const
{
    return _navigationMethod;
}

void ViewManager::containerViewsChanged(TabbedViewContainer *container)
{
    // TODO: Verify that this is right.
    emit viewPropertiesChanged(viewProperties());
}

void ViewManager::viewDestroyed(QWidget *view)
{
    // Note: the received QWidget has already been destroyed, so
    // using dynamic_cast<> or qobject_cast<> does not work here
    // We only need the pointer address to look it up below
    auto *display = reinterpret_cast<TerminalDisplay *>(view);

    // 1. detach view from session
    // 2. if the session has no views left, close it
    Session *session = _sessionMap[ display ];
    _sessionMap.remove(display);
    if (session != nullptr) {
        if (session->views().count() == 0) {
            session->close();
        }
    }

    //we only update the focus if the splitter is still alive
    updateDetachViewState();

    // The below causes the menus  to be messed up
    // Only happens when using the tab bar close button
//    if (_pluggedController)
//        emit unplugController(_pluggedController);
}

TerminalDisplay *ViewManager::createTerminalDisplay(Session *session)
{
    auto display = new TerminalDisplay(nullptr);
    display->setRandomSeed(session->sessionId() * 31);

    return display;
}

const ColorScheme *ViewManager::colorSchemeForProfile(const Profile::Ptr &profile)
{
    const ColorScheme *colorScheme = ColorSchemeManager::instance()->
                                     findColorScheme(profile->colorScheme());
    if (colorScheme == nullptr) {
        colorScheme = ColorSchemeManager::instance()->defaultColorScheme();
    }
    Q_ASSERT(colorScheme);

    return colorScheme;
}

bool ViewManager::profileHasBlurEnabled(const Profile::Ptr &profile)
{
    return colorSchemeForProfile(profile)->blur();
}

void ViewManager::applyProfileToView(TerminalDisplay *view, const Profile::Ptr &profile)
{
    Q_ASSERT(profile);
    view->applyProfile(profile);
    emit updateWindowIcon();
    emit blurSettingChanged(view->colorScheme()->blur());
}

void ViewManager::updateViewsForSession(Session *session)
{
    const Profile::Ptr profile = SessionManager::instance()->sessionProfile(session);

    const QList<TerminalDisplay *> sessionMapKeys = _sessionMap.keys(session);
    foreach (TerminalDisplay *view, sessionMapKeys) {
        applyProfileToView(view, profile);
    }
}

void ViewManager::profileChanged(const Profile::Ptr &profile)
{
    // update all views associated with this profile
    QHashIterator<TerminalDisplay *, Session *> iter(_sessionMap);
    while (iter.hasNext()) {
        iter.next();

        // if session uses this profile, update the display
        if (iter.key() != nullptr
            && iter.value() != nullptr
            && SessionManager::instance()->sessionProfile(iter.value()) == profile) {
            applyProfileToView(iter.key(), profile);
        }
    }
}

QList<ViewProperties *> ViewManager::viewProperties() const
{
    QList<ViewProperties *> list;

    TabbedViewContainer *container = _viewContainer;
    if (container == nullptr) {
        return {};
    }

    auto terminalContainers = _viewContainer->findChildren<TerminalDisplay*>();
    list.reserve(terminalContainers.size());

    for(auto terminalDisplay : _viewContainer->findChildren<TerminalDisplay*>()) {
        list.append(terminalDisplay->sessionController());
    }

    return list;
}

void ViewManager::saveSessions(KConfigGroup &group)
{
    // find all unique session restore IDs
    QList<int> ids;
    QSet<Session *> unique;
    int tab = 1;

    TabbedViewContainer *container = _viewContainer;

    // first: sessions in the active container, preserving the order
    Q_ASSERT(container);
    if (container == nullptr) {
        return;
    }
    ids.reserve(container->count());

    //TODO: Handle sessions
#if 0
    auto *activeview = qobject_cast<TerminalDisplay *>(container->currentWidget());
    for (int i = 0, end = container->count(); i < end; i++) {
        auto *view = qobject_cast<TerminalDisplay *>(container->widget(i));
        Q_ASSERT(view);

        Session *session = _sessionMap[view];
        ids << SessionManager::instance()->getRestoreId(session);
        unique.insert(session);
        if (view == activeview) {
            group.writeEntry("Active", tab);
        }
        tab++;
    }
#endif

    // second: all other sessions, in random order
    // we don't want to have sessions restored that are not connected
    foreach (Session *session, _sessionMap) {
        if (!unique.contains(session)) {
            ids << SessionManager::instance()->getRestoreId(session);
            unique.insert(session);
        }
    }

    group.writeEntry("Sessions", ids);
}

TabbedViewContainer *ViewManager::activeContainer()
{
    return _viewContainer;
}

void ViewManager::restoreSessions(const KConfigGroup &group)
{
    QList<int> ids = group.readEntry("Sessions", QList<int>());
    int activeTab = group.readEntry("Active", 0);
    TerminalDisplay *display = nullptr;

    int tab = 1;
    foreach (int id, ids) {
        Session *session = SessionManager::instance()->idToSession(id);

        if (session == nullptr) {
            qWarning() << "Unable to load session with id" << id;
            // Force a creation of a default session below
            ids.clear();
            break;
        }

        createView(session);
        if (!session->isRunning()) {
            session->run();
        }
        if (tab++ == activeTab) {
            display = qobject_cast<TerminalDisplay *>(activeView());
        }
    }

    if (display != nullptr) {
        activeContainer()->setCurrentWidget(display);
        display->setFocus(Qt::OtherFocusReason);
    }

    if (ids.isEmpty()) { // Session file is unusable, start default Profile
        Profile::Ptr profile = ProfileManager::instance()->defaultProfile();
        Session *session = SessionManager::instance()->createSession(profile);
        createView(session);
        if (!session->isRunning()) {
            session->run();
        }
    }
}

int ViewManager::sessionCount()
{
    return _sessionMap.size();
}

QStringList ViewManager::sessionList()
{
    QStringList ids;

    QHash<TerminalDisplay *, Session *>::const_iterator i;
    for (i = _sessionMap.constBegin(); i != _sessionMap.constEnd(); ++i) {
        ids.append(QString::number(i.value()->sessionId()));
    }

    return ids;
}

int ViewManager::currentSession()
{
    QHash<TerminalDisplay *, Session *>::const_iterator i;
    for (i = _sessionMap.constBegin(); i != _sessionMap.constEnd(); ++i) {
        if (i.key()->isVisible()) {
            return i.value()->sessionId();
        }
    }
    return -1;
}

void ViewManager::setCurrentSession(int sessionId)
{
    QHash<TerminalDisplay *, Session *>::const_iterator i;
    for (i = _sessionMap.constBegin(); i != _sessionMap.constEnd(); ++i) {
        if (i.value()->sessionId() == sessionId) {
            TabbedViewContainer *container = activeContainer();
            if (container != nullptr) {
                container->setCurrentWidget(i.key());
            }
        }
    }
}

int ViewManager::newSession()
{
    Profile::Ptr profile = ProfileManager::instance()->defaultProfile();
    Session *session = SessionManager::instance()->createSession(profile);

    session->addEnvironmentEntry(QStringLiteral("KONSOLE_DBUS_WINDOW=/Windows/%1").arg(managerId()));

    createView(session);
    session->run();

    return session->sessionId();
}

int ViewManager::newSession(const QString &profile)
{
    const QList<Profile::Ptr> profilelist = ProfileManager::instance()->allProfiles();
    Profile::Ptr profileptr = ProfileManager::instance()->defaultProfile();

    for (const auto &i : profilelist) {
        if (i->name() == profile) {
            profileptr = i;
            break;
        }
    }

    Session *session = SessionManager::instance()->createSession(profileptr);

    session->addEnvironmentEntry(QStringLiteral("KONSOLE_DBUS_WINDOW=/Windows/%1").arg(managerId()));

    createView(session);
    session->run();

    return session->sessionId();
}

int ViewManager::newSession(const QString &profile, const QString &directory)
{
    const QList<Profile::Ptr> profilelist = ProfileManager::instance()->allProfiles();
    Profile::Ptr profileptr = ProfileManager::instance()->defaultProfile();

    for (const auto &i : profilelist) {
        if (i->name() == profile) {
            profileptr = i;
            break;
        }
    }

    Session *session = SessionManager::instance()->createSession(profileptr);
    session->setInitialWorkingDirectory(directory);

    session->addEnvironmentEntry(QStringLiteral("KONSOLE_DBUS_WINDOW=/Windows/%1").arg(managerId()));

    createView(session);
    session->run();

    return session->sessionId();
}

QString ViewManager::defaultProfile()
{
    return ProfileManager::instance()->defaultProfile()->name();
}

QStringList ViewManager::profileList()
{
    return ProfileManager::instance()->availableProfileNames();
}

void ViewManager::nextSession()
{
    nextView();
}

void ViewManager::prevSession()
{
    previousView();
}

void ViewManager::moveSessionLeft()
{
    moveActiveViewLeft();
}

void ViewManager::moveSessionRight()
{
    moveActiveViewRight();
}

void ViewManager::setTabWidthToText(bool setTabWidthToText)
{
    _viewContainer->tabBar()->setExpanding(!setTabWidthToText);
    _viewContainer->tabBar()->update();
}

void ViewManager::setNavigationVisibility(NavigationVisibility navigationVisibility) {
    if (_navigationVisibility != navigationVisibility) {
        _navigationVisibility = navigationVisibility;
        _viewContainer->setNavigationVisibility(navigationVisibility);
    }
}

void ViewManager::setNavigationBehavior(int behavior)
{
    _newTabBehavior = static_cast<NewTabBehavior>(behavior);
}

void ViewManager::updateTerminalDisplayHistory(TerminalDisplay* terminalDisplay, bool remove)
{
    if (terminalDisplay == nullptr) {
        if (_terminalDisplayHistoryIndex >= 0) {
            // This is the case when we finished walking through the history
            // (i.e. when Ctrl-Tab has been released)
            terminalDisplay = _terminalDisplayHistory[_terminalDisplayHistoryIndex];
            _terminalDisplayHistoryIndex = -1;
        } else {
            return;
        }
    }

    if (_terminalDisplayHistoryIndex >= 0 && !remove) {
        // Do not reorder the tab history while we are walking through it
        return;
    }

    for (int i = 0; i < _terminalDisplayHistory.count(); i++) {
        if (_terminalDisplayHistory[i] == terminalDisplay) {
            _terminalDisplayHistory.removeAt(i);
            if (!remove) {
                _terminalDisplayHistory.prepend(terminalDisplay);
            }
            break;
        }
    }
}
