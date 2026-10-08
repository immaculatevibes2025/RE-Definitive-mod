#include "SaturnTab.h"

#include "RunPanel.h"
#include "Worker.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

const char* const kExtractorName = "RE1 Saturn Extractor.exe";

QWidget* pathRow(QLineEdit* edit, QPushButton* browse) {
    auto* w = new QWidget();
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(edit, 1);
    l->addWidget(browse);
    return w;
}

// The extractor, next to this program (or in a saturn/ subfolder).
QString findExtractor() {
    const QDir dir(QCoreApplication::applicationDirPath());
    for (const QString& p : {dir.filePath(kExtractorName),
                             dir.filePath(QStringLiteral("saturn/") + kExtractorName)}) {
        if (QFileInfo::exists(p)) return p;
    }
    return QString();
}

}  // namespace

SaturnTab::SaturnTab(QWidget* parent) : QWidget(parent) {
    m_image = new QLineEdit(this);
    auto* browseImage = new QPushButton(tr("Browse..."), this);

    m_target = new QLineEdit(this);
    m_target->setText(QCoreApplication::applicationDirPath());
    auto* browseTarget = new QPushButton(tr("Browse..."), this);

    auto* form = new QFormLayout();
    form->addRow(tr("Saturn disc image:"), pathRow(m_image, browseImage));
    form->addRow(tr("Resident Evil PC game folder:"), pathRow(m_target, browseTarget));

    m_run = new RunPanel(this);

    auto* note = new QLabel(
        tr("Builds the Sega Saturn extras from your own Resident Evil (Saturn) "
           "disc image (.cue, .bin or .iso): the Ticks, Zombie Wesker, the "
           "Saturn outfits, the Battle Game rooms and music and the Tick "
           "sounds, plus the title-menu and Option Mode art made from the PC "
           "files. Everything is written into <game folder>/USA.\n\n"
           "Run the PC Assets tab first. This tab uses \"%1\", which must be "
           "next to this program.").arg(QString::fromLatin1(kExtractorName)),
        this);
    note->setWordWrap(true);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note);
    layout->addLayout(form);
    layout->addWidget(m_run, 1);

    connect(browseImage, &QPushButton::clicked, this, &SaturnTab::onBrowseImage);
    connect(browseTarget, &QPushButton::clicked, this, &SaturnTab::onBrowseTarget);

    m_run->setTaskFactory([this]() -> Worker* {
        const QString image = m_image->text();
        const QString target = m_target->text();
        return new Worker([image, target](const re1::Progress& p, QString& error) {
            const QString exe = findExtractor();
            if (exe.isEmpty()) {
                error = QStringLiteral("%1 was not found next to the migrator.")
                            .arg(QString::fromLatin1(kExtractorName));
                return false;
            }
            if (image.isEmpty() || target.isEmpty()) {
                error = QStringLiteral("Pick the Saturn disc image and the game folder first.");
                return false;
            }
            // The extractor runs its command-line mode with two arguments and
            // prints one line per step.
            QProcess proc;
            proc.setProcessChannelMode(QProcess::MergedChannels);
            proc.start(exe, {image, target});
            if (!proc.waitForStarted(15000)) {
                error = QStringLiteral("Could not start %1.").arg(exe);
                return false;
            }
            p.phase(-1, -1, "Extracting");
            QString lastLine;
            QByteArray pending;
            while (proc.state() != QProcess::NotRunning) {
                if (p.isCancelled()) {
                    proc.kill();
                    proc.waitForFinished(5000);
                    error = QStringLiteral("Cancelled.");
                    return false;
                }
                proc.waitForReadyRead(200);
                pending += proc.readAll();
                int nl;
                while ((nl = pending.indexOf('\n')) >= 0) {
                    const QString line = QString::fromUtf8(pending.left(nl)).trimmed();
                    pending.remove(0, nl + 1);
                    if (!line.isEmpty()) {
                        p.info(line.toStdString());
                        lastLine = line;
                    }
                }
            }
            pending += proc.readAll();
            const QString rest = QString::fromUtf8(pending).trimmed();
            if (!rest.isEmpty()) {
                p.info(rest.toStdString());
                lastLine = rest;
            }
            if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
                error = lastLine.isEmpty() ? QStringLiteral("The extractor failed.") : lastLine;
                return false;
            }
            return true;
        });
    });
}

void SaturnTab::onBrowseImage() {
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Select the Resident Evil (Saturn) disc image"), m_image->text(),
        tr("Disc images (*.cue *.bin *.iso);;All files (*)"));
    if (!f.isEmpty()) m_image->setText(f);
}

void SaturnTab::onBrowseTarget() {
    const QString d = QFileDialog::getExistingDirectory(
        this, tr("Select the Resident Evil PC game folder"), m_target->text());
    if (!d.isEmpty()) m_target->setText(d);
}
