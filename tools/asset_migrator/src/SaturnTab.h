#pragma once
// "Saturn Extras" tab (fork addition): runs RE1 Saturn Extractor.exe, shipped
// next to this program, to build the Sega Saturn extras (Ticks, Zombie Wesker,
// Saturn outfits, Battle Game rooms and music, Tick sounds) from the player's
// own Saturn disc image. See tools/saturn/saturn_extractor.py.

#include <QWidget>

class QLineEdit;
class RunPanel;

class SaturnTab : public QWidget {
    Q_OBJECT
public:
    explicit SaturnTab(QWidget* parent = nullptr);

private slots:
    void onBrowseImage();
    void onBrowseTarget();

private:
    QLineEdit* m_image = nullptr;
    QLineEdit* m_target = nullptr;
    RunPanel* m_run = nullptr;
};
