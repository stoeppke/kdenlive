/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "test_utils.hpp"
// test specific headers
#include "doc/kdenlivedoc.h"
#include "timeline2/model/timelinefunctions.hpp"

#include <QStandardPaths>
#include <QUndoGroup>
#include <mlt++/MltFrame.h>

using namespace fakeit;

namespace {
QColor colorAt(const std::shared_ptr<TimelineItemModel> &timeline, int position)
{
    timeline->tractor()->seek(position);
    std::unique_ptr<Mlt::Frame> frame(timeline->tractor()->get_frame());
    mlt_image_format format = mlt_image_rgb;
    int width = 16;
    int height = 16;
    const auto *image = frame->get_image(format, width, height);
    if (!image || format != mlt_image_rgb) {
        return QColor();
    }
    // Sample the center: the loader normalizes the color clips, which letterboxes them to the profile's display aspect ratio
    const auto *pixel = image + 3 * ((height / 2) * width + width / 2);
    return QColor(pixel[0], pixel[1], pixel[2]);
}

/** @brief Names the colors used by the test angles, so that a failure shows the actual color */
std::string colorName(const QColor &color)
{
    if (color.red() > 200 && color.green() < 60 && color.blue() < 60) {
        return "red";
    }
    if (color.red() > 200 && color.green() > 200 && color.blue() < 60) {
        return "yellow";
    }
    if (color.red() < 30 && color.green() < 30 && color.blue() < 30) {
        return "black";
    }
    return color.name().toStdString();
}

/** @brief Creates a color clip through the MLT loader, as the application does. The loader's normalizers convert the clip's rgb image to the
 *  rgba requested by the track compositing; a bare color producer has no converter and returns rgb data read as rgba. */
QString createColorClip(const std::shared_ptr<ProjectItemModel> &binModel, const std::string &color, int length)
{
    auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), nullptr, ("color:" + color).c_str());
    REQUIRE(producer->is_valid());
    producer->set("length", length);
    producer->set("out", length - 1);
    QString binId;
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    REQUIRE(binModel->requestAddBinClip(binId, producer, QStringLiteral("-1"), undo, redo));
    binModel->getClipByBinID(binId)->forceLimitedDuration();
    return binId;
}

QUuid sequenceUuid(const std::shared_ptr<ProjectItemModel> &binModel, const QString &binId)
{
    QMap<QUuid, QString> allSequences = binModel->getAllSequenceClips();
    return allSequences.key(binId);
}
} // namespace

TEST_CASE("Multicam angles of a sequence clip", "[Multicam]")
{
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    std::shared_ptr<DocUndoStack> undoStack = std::make_shared<DocUndoStack>(nullptr);
    const QString saveFile = QDir::temp().absoluteFilePath(QStringLiteral("test-multicam.kdenlive"));

    SECTION("Show single angles, cut to an angle, undo and save")
    {
        pCore->setCurrentProfile(QStringLiteral("dv_pal"));
        KdenliveDoc document(undoStack);
        Mock<KdenliveDoc> docMock(document);
        KdenliveDoc &mockedDoc = docMock.get();
        pCore->projectManager()->testSetDocument(&mockedDoc);
        QDateTime documentDate = QDateTime::currentDateTime();
        KdenliveTests::updateTimeline(false, QString(), QString(), documentDate, 0);
        auto mainTimeline = mockedDoc.getTimeline(mockedDoc.uuid());
        pCore->projectManager()->testSetActiveTimeline(mainTimeline);
        KdenliveTests::resetNextId();

        // Two cameras: red on V1 and yellow on V2 of the multicam sequence
        QString redId = createColorClip(binModel, "0xff0000ff", 100);
        QString yellowId = createColorClip(binModel, "0xffff00ff", 100);
        const QString seqId = ClipCreator::createPlaylistClip(QStringLiteral("Multicam"), {1, 2}, QStringLiteral("-1"), binModel);
        REQUIRE(seqId != QLatin1String("-1"));
        auto sequence = mockedDoc.getTimeline(sequenceUuid(binModel, seqId));
        REQUIRE(sequence);
        int v1 = sequence->getTrackIndexFromPosition(1);
        int v2 = sequence->getTrackIndexFromPosition(2);
        sequence->setTrackName(v2, QStringLiteral("Side"));
        int redClip = -1;
        int yellowClip = -1;
        REQUIRE(sequence->requestClipInsertion(redId, v1, 0, redClip, true, true, false));
        REQUIRE(sequence->requestClipInsertion(yellowId, v2, 0, yellowClip, true, true, false));
        // The full sequence shows the top track
        REQUIRE(colorName(colorAt(sequence, 10)) == "yellow");

        std::shared_ptr<ProjectClip> sequenceClip = binModel->getClipByBinID(seqId);
        REQUIRE(sequenceClip->multicamAngles() == QStringList({QStringLiteral("V1"), QStringLiteral("Side")}));

        // Insert the sequence in the main timeline
        pCore->projectManager()->testSetActiveTimeline(mainTimeline);
        int tid = mainTimeline->getTrackIndexFromPosition(2);
        KdenliveTests::setVideoTargets(mainTimeline, tid);
        int cid = -1;
        REQUIRE(mainTimeline->requestClipInsertion(seqId, tid, 0, cid, true, true, false));
        REQUIRE(KdenliveTests::getClipPtr(mainTimeline, cid)->multicamAngle() == -1);
        REQUIRE(colorName(colorAt(mainTimeline, 10)) == "yellow");

        // Show the first angle only
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        REQUIRE(TimelineFunctions::setMulticamAngle(mainTimeline, cid, 0, undo, redo));
        pCore->pushUndo(undo, redo, QStringLiteral("Switch angle"));
        REQUIRE(KdenliveTests::getClipPtr(mainTimeline, cid)->multicamAngle() == 0);
        REQUIRE(KdenliveTests::getClipPtr(mainTimeline, cid)->multicamAngleName() == QStringLiteral("1: V1"));
        REQUIRE(colorName(colorAt(mainTimeline, 10)) == "red");
        REQUIRE(colorName(colorAt(mainTimeline, 60)) == "red");

        // Cut to the second angle at frame 50
        REQUIRE(TimelineFunctions::requestMulticamCut(mainTimeline, cid, 50, 1));
        auto cutState = [&]() {
            REQUIRE(mainTimeline->checkConsistency());
            REQUIRE(mainTimeline->getTrackClipsCount(tid) == 2);
            int left = mainTimeline->getClipByPosition(tid, 10);
            int right = mainTimeline->getClipByPosition(tid, 60);
            REQUIRE(left == cid);
            REQUIRE(mainTimeline->getClipPosition(right) == 50);
            REQUIRE(mainTimeline->getClipIn(right) == 50);
            REQUIRE(KdenliveTests::getClipPtr(mainTimeline, left)->multicamAngle() == 0);
            REQUIRE(KdenliveTests::getClipPtr(mainTimeline, right)->multicamAngle() == 1);
            REQUIRE(KdenliveTests::getClipPtr(mainTimeline, right)->multicamAngleName() == QStringLiteral("2: Side"));
            REQUIRE(colorName(colorAt(mainTimeline, 10)) == "red");
            REQUIRE(colorName(colorAt(mainTimeline, 60)) == "yellow");
        };
        cutState();

        undoStack->undo();
        REQUIRE(mainTimeline->getTrackClipsCount(tid) == 1);
        REQUIRE(KdenliveTests::getClipPtr(mainTimeline, cid)->multicamAngle() == 0);
        REQUIRE(colorName(colorAt(mainTimeline, 60)) == "red");
        undoStack->redo();
        cutState();

        // A gap in the angle track shows black, not the other angle
        REQUIRE(sequence->requestItemResize(redClip, 80, true) == 80);
        int right = mainTimeline->getClipByPosition(tid, 60);
        Fun undoAngle = []() { return true; };
        Fun redoAngle = []() { return true; };
        REQUIRE(TimelineFunctions::setMulticamAngle(mainTimeline, right, 0, undoAngle, redoAngle));
        REQUIRE(colorName(colorAt(mainTimeline, 90)) == "black");
        REQUIRE(undoAngle());
        REQUIRE(colorName(colorAt(mainTimeline, 90)) == "yellow");

        REQUIRE(pCore->projectManager()->testSaveFileAs(saveFile));
        // Release the references before closing the document: the bin clip destructor needs the current document
        sequenceClip.reset();
        sequence.reset();
        mainTimeline.reset();
        pCore->projectManager()->closeCurrentDocument(false, false);
    }

    SECTION("Reopen and check angles")
    {
        KdenliveTests::resetNextId();
        QUrl openURL = QUrl::fromLocalFile(saveFile);
        Mock<ProjectManager> pmMock;
        When(Method(pmMock, undoStack)).AlwaysReturn(undoStack);
        When(Method(pmMock, cacheDir)).AlwaysReturn(QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)));
        QUndoGroup *undoGroup = new QUndoGroup();
        undoGroup->addStack(undoStack.get());
        DocOpenResult openResults = KdenliveDoc::Open(openURL, QDir::temp().path(), undoGroup, false, nullptr);
        REQUIRE(openResults.isSuccessful() == true);
        std::unique_ptr<KdenliveDoc> openedDoc = openResults.getDocument();
        pCore->projectManager()->testSetDocument(openedDoc.get());
        const QUuid uuid = openedDoc->uuid();
        QDateTime documentDate = QFileInfo(openURL.toLocalFile()).lastModified();
        KdenliveTests::updateTimeline(false, QString(), QString(), documentDate, 0);
        QMap<QUuid, QString> allSequences = binModel->getAllSequenceClips();
        const QString mainId = allSequences.take(uuid);
        pCore->projectManager()->openTimeline(mainId, -1, uuid);
        for (auto it = allSequences.cbegin(); it != allSequences.cend(); ++it) {
            pCore->projectManager()->openTimeline(it.value(), -1, it.key());
        }
        auto mainTimeline = openedDoc->getTimeline(uuid);
        pCore->projectManager()->testSetActiveTimeline(mainTimeline);

        int tid = mainTimeline->getTrackIndexFromPosition(2);
        REQUIRE(mainTimeline->getTrackClipsCount(tid) == 2);
        int left = mainTimeline->getClipByPosition(tid, 10);
        int right = mainTimeline->getClipByPosition(tid, 60);
        REQUIRE(KdenliveTests::getClipPtr(mainTimeline, left)->multicamAngle() == 0);
        REQUIRE(KdenliveTests::getClipPtr(mainTimeline, right)->multicamAngle() == 1);
        REQUIRE(colorName(colorAt(mainTimeline, 10)) == "red");
        REQUIRE(colorName(colorAt(mainTimeline, 60)) == "yellow");
        REQUIRE(mainTimeline->checkConsistency());

        mainTimeline.reset();
        pCore->projectManager()->closeCurrentDocument(false, false);
        QFile::remove(saveFile);
    }
}
