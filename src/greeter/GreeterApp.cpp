/***************************************************************************
* Copyright (c) 2015 Pier Luigi Fiorini <pierluigi.fiorini@gmail.com>
* Copyright (c) 2013 Abdurrahman AVCI <abdurrahmanavci@gmail.com>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the
* Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
***************************************************************************/

#include "GreeterApp.h"
#include "Configuration.h"
#include "GreeterProxy.h"
#include "Constants.h"
#include "ScreenModel.h"
#include "SessionModel.h"
#include "ThemeConfig.h"
#include "ThemeMetadata.h"
#include "UserModel.h"
#include "KeyboardModel.h"

#include "MessageHandler.h"

#include <QGuiApplication>
#include <QQuickView>
#include <QQmlContext>
#include <QQmlEngine>
#include <QDebug>
#include <QTranslator>

#include <iostream>

namespace SDDM {
    QString parameter(const QStringList &arguments, const QString &key, const QString &defaultValue) {
        int index = arguments.indexOf(key);

        if ((index < 0) || (index >= arguments.size() - 1))
            return defaultValue;

        QString value = arguments.at(index + 1);

        if (value.startsWith(QLatin1Char('-')))
            return defaultValue;

        return value;
    }

    GreeterApp *GreeterApp::self = nullptr;

    GreeterApp::GreeterApp(int &argc, char **argv) : QGuiApplication(argc, argv) {
        // point instance to this
        self = this;

        // Parse arguments
        bool testing = false;

        if (arguments().contains(QStringLiteral("--test-mode")))
            testing = true;

        // get socket name
        QString socket = parameter(arguments(), QStringLiteral("--socket"), QString());

        // get theme path
        m_themePath = parameter(arguments(), QStringLiteral("--theme"), QString());

        // read theme metadata
        m_metadata = new ThemeMetadata(QStringLiteral("%1/metadata.desktop").arg(m_themePath));

        // Translations
        // Components translation
        m_components_tranlator = new QTranslator();
        if (m_components_tranlator->load(QLocale::system(), QString(), QString(), QStringLiteral(COMPONENTS_TRANSLATION_DIR)))
            installTranslator(m_components_tranlator);

        // Theme specific translation
        m_theme_translator = new QTranslator();
        if (m_theme_translator->load(QLocale::system(), QString(), QString(),
                           QStringLiteral("%1/%2/").arg(m_themePath, m_metadata->translationsDirectory())))
            installTranslator(m_theme_translator);

        // get theme config file
        QString configFile = QStringLiteral("%1/%2").arg(m_themePath).arg(m_metadata->configFile());

        // read theme config
        m_themeConfig = new ThemeConfig(configFile);

        // set default icon theme from greeter theme
        if (m_themeConfig->contains(QStringLiteral("iconTheme")))
            QIcon::setThemeName(m_themeConfig->value(QStringLiteral("iconTheme")).toString());

        // set cursor theme according to greeter theme
        if (m_themeConfig->contains(QStringLiteral("cursorTheme")))
            qputenv("XCURSOR_THEME", m_themeConfig->value(QStringLiteral("cursorTheme")).toString().toUtf8());

        // set platform theme
        if (m_themeConfig->contains(QStringLiteral("platformTheme")))
            qputenv("QT_QPA_PLATFORMTHEME", m_themeConfig->value(QStringLiteral("platformTheme")).toString().toUtf8());

        // create models

        m_sessionModel = new SessionModel();
        m_screenModel = new ScreenModel();
        m_userModel = new UserModel();
        m_proxy = new GreeterProxy(socket);
        m_keyboard = new KeyboardModel();

        if(!testing && !m_proxy->isConnected()) {
            qCritical() << "Cannot connect to the daemon - is it running?";
            exit(EXIT_FAILURE);
        }

        // Set numlock upon start
        if (m_keyboard->enabled()) {
            if (mainConfig.Numlock.get() == MainConfig::NUM_SET_ON)
                m_keyboard->setNumLockState(true);
            else if (mainConfig.Numlock.get() == MainConfig::NUM_SET_OFF)
                m_keyboard->setNumLockState(false);
        }

        m_proxy->setSessionModel(m_sessionModel);

        // create views
        QList<QScreen *> screens = primaryScreen()->virtualSiblings();
        Q_FOREACH (QScreen *screen, screens)
            addViewForScreen(screen);

        // handle screens
        connect(this, &GreeterApp::screenAdded, this, &GreeterApp::addViewForScreen);
    }

    void GreeterApp::addViewForScreen(QScreen *screen) {
        // create view
        QQuickView *view = new QQuickView();
        view->setScreen(screen);
        view->setResizeMode(QQuickView::SizeRootObjectToView);
        //view->setGeometry(QRect(QPoint(0, 0), screen->geometry().size()));
        view->setGeometry(screen->geometry());

        // remove the view when the screen is removed, but we
        // need to be careful here since Qt will move the view to
        // another screen before this signal is emitted so we
        // pass a pointer to the view to our slot
#if (QT_VERSION >= QT_VERSION_CHECK(5, 4, 0))
        connect(this, &GreeterApp::screenRemoved, this, [view, this](QScreen *) {
            removeViewForScreen(view);
        });
#else
        connect(view, &QQuickView::screenChanged, this, [view, this](QScreen *screen) {
            if (screen == Q_NULLPTR)
                removeViewForScreen(view);
        });
#endif

#if (QT_VERSION >= QT_VERSION_CHECK(5, 4, 0))
        // always resize when the screen geometry changes
        connect(screen, &QScreen::geometryChanged, this, [view](const QRect &r) {
            view->setGeometry(r);
        });
#endif

        view->engine()->addImportPath(QStringLiteral(IMPORTS_INSTALL_DIR));

        // connect proxy signals
        connect(m_proxy, SIGNAL(loginSucceeded()), view, SLOT(close()));

        // set context properties
        view->rootContext()->setContextProperty(QStringLiteral("sessionModel"), m_sessionModel);
        view->rootContext()->setContextProperty(QStringLiteral("screenModel"), m_screenModel);
        view->rootContext()->setContextProperty(QStringLiteral("userModel"), m_userModel);
        view->rootContext()->setContextProperty(QStringLiteral("config"), *m_themeConfig);
        view->rootContext()->setContextProperty(QStringLiteral("sddm"), m_proxy);
        view->rootContext()->setContextProperty(QStringLiteral("keyboard"), m_keyboard);

        // get theme main script
        QString mainScript = QStringLiteral("%1/%2").arg(m_themePath).arg(m_metadata->mainScript());

        // set main script as source
        view->setSource(QUrl::fromLocalFile(mainScript));

        // show
        qDebug() << "Adding view for" << screen->name() << screen->geometry();
        view->show();
    }

    void GreeterApp::removeViewForScreen(QQuickView *view) {
        m_views.removeOne(view);
        view->deleteLater();
    }
}

int main(int argc, char **argv) {
    // install message handler
    qInstallMessageHandler(SDDM::GreeterMessageHandler);

    QStringList arguments;

    for (int i = 0; i < argc; i++)
        arguments << QString::fromLocal8Bit(argv[i]);

    if (arguments.contains(QStringLiteral("--help")) || arguments.contains(QStringLiteral("-h"))) {
        std::cout << "Usage: " << argv[0] << " [options] [arguments]\n"
                     "Options: \n"
                     "  --theme <theme path>       Set greeter theme\n"
                     "  --socket <socket name>     Set socket name\n"
                     "  --test-mode                Start greeter in test mode" << std::endl;

        return EXIT_FAILURE;
    }

    SDDM::GreeterApp app(argc, argv);

    return app.exec();
}
