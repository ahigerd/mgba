/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "PopupManager.h"

#include <QDialog>

#include "CoreController.h"

using namespace QGBA;

PopupManagerBase::PopupManagerBase(PopupManagerBase::Private* d)
	: m_d(d)
{
	// initializers only
}

void PopupManagerBase::show() {
	QWidget* w = construct();
	if (!w) {
		return;
	}
	w->show();
	w->activateWindow();
	w->raise();
}

QWidget* PopupManagerBase::construct() {
	QWidget* w = d()->window();
	if (w) {
		return w;
	}
	constructImpl();
	w = d()->window();
	if (w && d()->keepAlive) {
		w->setAttribute(Qt::WA_DeleteOnClose);
	}
	if (d()->m_controller) {
		d()->onCoreAttached(d()->m_controller.getShared());
	}
	return w;
}

void PopupManagerBase::Private::onCoreDetached(std::shared_ptr<CoreController>) {
	if (stopConnection) {
		QObject::disconnect(stopConnection);
		stopConnection = QMetaObject::Connection();
	}
}

void PopupManagerBase::Private::onCoreAttached(std::shared_ptr<CoreController> controller) {
	QWidget* widget = window();
	if (widget) {
		stopConnection = QObject::connect(controller.get(), &CoreController::stopping, widget, &QWidget::close);
	}
}
