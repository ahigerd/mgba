/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QWidget>

#include <functional>
#include <memory>

#include "CheatsModel.h"
#include "CorePointer.h"
#include "ui_CheatsView.h"

struct mCheatDevice;

namespace QGBA {

class CoreController;

class CheatsView : public QWidget, public CoreConsumer {
Q_OBJECT

public:
	CheatsView(CorePointerSource* controller, QWidget* parent = nullptr);

	virtual bool eventFilter(QObject*, QEvent*) override;

private slots:
	void load();
	void save();
	void addSet();
	void removeSet();
	void enterCheat();

private:
	void registerCodeType(const QString& label, int type);

	Ui::CheatsView m_ui;
	CheatsModel m_model;
	QButtonGroup* m_typeGroup = nullptr;

	int m_codeType = 0;
};

}
