/**
 * @file
 */
#pragma once

#include "IDialog.h"
#include <QGroupBox>

namespace ai {
namespace debug {

class SettingsDialog : public IDialog {
Q_OBJECT
private:
	QGroupBox* createMapView();

private slots:
	void setShowGrid(int value);
	void setGridInterval(const QString& value);
	void setItemSize(const QString& value);

public:
	SettingsDialog();

	void addMainWidgets(QBoxLayout& layout) override;
};

}
}
