#include "device/QuickReply.h"

#include <QtTest>

using namespace rl::quickreply;
using Json = nlohmann::json;

class TestQuickReply : public QObject
{
    Q_OBJECT
private slots:
    void httpList_readsIdsAndNames()
    {
        const Json v = Json::parse(R"({"AudioFileList":[{"id":3,"fileName":"Leave it"},{"id":7,"fileName":"Wait"}]})");
        const ClipList l = parseHttpList(v);
        QCOMPARE(l.clips.size(), 2);
        QCOMPARE(l.clips[0].id, 3);
        QCOMPARE(l.clips[0].name, QString("Leave it"));
        QCOMPARE(l.clips[1].id, 7);
    }
    void httpList_nullOrMissingIsEmpty()
    {
        QVERIFY(parseHttpList(Json::parse(R"({"AudioFileList":null})")).clips.isEmpty());
        QVERIFY(parseHttpList(Json::parse(R"({})")).clips.isEmpty());
        QVERIFY(parseHttpList(Json()).clips.isEmpty());
    }
    void httpList_dropsEntriesWithoutId()
    {
        const Json v = Json::parse(R"({"AudioFileList":[{"fileName":"x"},{"id":0,"fileName":"ok"}]})");
        const ClipList l = parseHttpList(v);
        QCOMPARE(l.clips.size(), 1);
        QCOMPARE(l.clips[0].id, 0); // id 0 is a valid clip id
    }
    void baichuanList_readsClipsAndCapacity()
    {
        const QByteArray xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" ?><body><audioFileInfoList version=\"1.1\">"
            "<maxFileNumber>12</maxFileNumber><remainFileTotalDuration>40</remainFileTotalDuration>"
            "<audioFileInfo><id>1</id><fileName>Hello</fileName><duration>3</duration></audioFileInfo>"
            "<audioFileInfo><id>2</id><fileName>Bye</fileName></audioFileInfo>"
            "</audioFileInfoList></body>";
        const ClipList l = parseBaichuanList(xml);
        QCOMPARE(l.maxFiles, 12);
        QCOMPARE(l.clips.size(), 2);
        QCOMPARE(l.clips[1].id, 2);
        QCOMPARE(l.clips[1].name, QString("Bye"));
    }
    // The real Woorabinda doorbell reply: capacity but no clips.
    void baichuanList_emptyWithCapacity()
    {
        const QByteArray xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<audioFileInfoList version=\"1.1\">\n"
            "<maxFileNumber>12</maxFileNumber>\n<remainFileTotalDuration>0</remainFileTotalDuration>\n"
            "</audioFileInfoList>\n</body>\n";
        const ClipList l = parseBaichuanList(xml);
        QVERIFY(l.clips.isEmpty());
        QCOMPARE(l.maxFiles, 12);
    }
    void baichuanList_garbageIsEmpty()
    {
        QVERIFY(parseBaichuanList("").clips.isEmpty());
        QVERIFY(parseBaichuanList("not xml").clips.isEmpty());
    }
    void playBody_carriesChannelAndId()
    {
        const QByteArray b = baichuanPlayBody(3, 5);
        QVERIFY(b.contains("<channelId>3</channelId>"));
        QVERIFY(b.contains("<id>5</id>"));
        QVERIFY(b.contains("<timeout>0</timeout>"));
    }
    void fallbackCodes()
    {
        for (int c : {-4, -9, -12, -13, -17})
            QVERIFY(httpShouldFallBack(c));
        QVERIFY(!httpShouldFallBack(0));
        QVERIFY(!httpShouldFallBack(-1));
    }
};

QTEST_GUILESS_MAIN(TestQuickReply)
#include "test_quickreply.moc"
