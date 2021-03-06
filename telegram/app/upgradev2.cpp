#include "upgradev2.h"

#include <QDebug>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QIODevice>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>

#include <telegram/types/peer.h>
#include <telegram/types/messageaction.h>
#include <telegram/types/messagemedia.h>

UpgradeV2::UpgradeV2(QObject *parent) : QObject(parent) {
    phone = "";
    configPath = QDir::homePath() + "/.config/" + QCoreApplication::organizationDomain().toLower();
    cachePath  = QDir::homePath() + "/.cache/" + QCoreApplication::organizationDomain().toLower();
    configFilePath = configPath + "/com.ubuntu.telegram.conf";

    config.setPath(configPath);

    localId = 1; // This a fake localId value, used as an incremental counter.
}

UpgradeV2::~UpgradeV2() {
}

void UpgradeV2::upgrade() {
    getPhoneNumber();
    if (phone.isEmpty()) {
        qDebug() << TAG << "not signed in to v1";
        return;
    }
    if (!QFile(configFilePath).exists()) {
        qDebug() << TAG << "nothing to do for v1";
        return;
    }

    setUpPhonePaths();

    copyDatabaseFiles();

    createConfig();

    copySecretChats();

    insertProfile();

    deleteFiles();

    qDebug() << TAG << "upgraded to v2";
}

void UpgradeV2::getPhoneNumber() {
    QDir dir(configPath);
    dir.setFilter(QDir::Dirs);
    dir.setSorting(QDir::Name);
    QRegExp re("^\\+\\d{1,2}\\d+$");

    QStringList entries = dir.entryList();
    for (QString entry: entries) {
        if (re.indexIn(entry) == 0) {
            phone = entry;
            break;
        }
    }
}

void UpgradeV2::setUpPhonePaths() {
    configPhonePath = configPath + "/" + phone;
    cachePhonePath  = cachePath + "/" + phone;

    databaseFilePath = cachePhonePath + "/telegram.sqlite";

    QDir configPhoneDir(configPhonePath);

    newProfilesPath = config.absoluteFilePath("profiles.sqlite");
    newUserdataPath = configPhoneDir.absoluteFilePath("userdata.db");
    newDatabasePath = configPhoneDir.absoluteFilePath("database.db");
}

void UpgradeV2::copyFromResource(QString resource, QString path) {
    QFile dest(path);
    if (!dest.exists()) {
        QFile::copy(resource, path);
    }
    dest.setPermissions(QFileDevice::WriteOwner|QFileDevice::WriteGroup|QFileDevice::ReadUser|QFileDevice::ReadGroup);
}

void UpgradeV2::copyDatabaseFiles() {
    copyFromResource(DATABASE_DB_PATH, newDatabasePath);
    copyFromResource(USERDATA_DB_PATH, newUserdataPath);
    copyFromResource(PROFILES_DB_PATH, newProfilesPath);
}

void UpgradeV2::createConfig() {
    /*
    // This is optional, as default config values are re-created.
    QString newConfigFilePath(config.absoluteFilePath("config.ini"));
    qDebug() << "upgrade0: " << newConfigFilePath;
    QFile configFile(newConfigFilePath);
    if (configFile.open(QIODevice::WriteOnly)) {
        QTextStream out(&configFile);
        out << "[%General]" << endl;
        out << "pushNotifications=true" << endl;
        out << "pushNumber=" << phone << endl;
        out.flush();
    }
    */
}

inline void UpgradeV2::copySecretPhoto(qint64 peer, bool /*out*/, qint64 mediaId, QSqlDatabase &newDb) {
    QSqlQuery photo(db);
    photo.prepare("SELECT caption, date, accessHash, userId FROM mediaPhotos WHERE id = :id");
    photo.bindValue(":id", mediaId);
    if (!photo.exec()) {
        qCritical() << TAG << "failed to get secret photo" << photo.lastError();
        return;
    }
    if (photo.next()) {
        QSqlQuery newPhoto(newDb);

        qint64 accessHash = photo.value("accessHash").toLongLong();
        if (accessHash == 0) accessHash = 1;

        newPhoto.prepare("INSERT INTO Photos (id, caption, date, accessHash, userId) "
                "VALUES (:id, :caption, :date, :accessHash, :userId)");
        newPhoto.bindValue(":id", mediaId);
        newPhoto.bindValue(":caption", photo.value("caption").toString());
        newPhoto.bindValue(":date", photo.value("date").toLongLong());
        newPhoto.bindValue(":accessHash", accessHash);
        newPhoto.bindValue(":userId", photo.value("userId").toLongLong());

        if (!newPhoto.exec()) {
            qCritical() << TAG << "failed to insert secret photo" << newPhoto.lastError();
            return;
        }
    } else {
        qCritical() << TAG << "secret photo not found";
        return;
    }

    QSqlQuery query(db);
    query.prepare("SELECT photoId, type, size, width, height, dcId, localId, secret, volumeId, localPath "
            "FROM photoSizes JOIN fileLocations ON photoSizes.fileLocationId = fileLocations.rowid "
            "WHERE photoId = :photoId");
    query.bindValue(":photoId", mediaId);
    if (!query.exec()) {
        qCritical() << TAG << "failed to get photo sizes for secret photo" << query.lastError();
        return;
    }

    QString newPath = QString("%1/%2/downloads/%3").arg(cachePath).arg(phone).arg(peer);

    QDir dir;
    dir.mkpath(newPath);

    QSqlQuery insert(newDb);
    for ( ; query.next(); localId++) {
        // Commented fields left intentionally to show missing metadata for secret chat attachments.
        qint64 pid      = query.value("photoId").toLongLong();
        QString type    = query.value("type").toString();
        qint64 size     = query.value("size").toLongLong();
        qint64 w        = query.value("width").toLongLong();
        qint64 h        = query.value("height").toLongLong();
        // qint64 localId  = query.value("localId").toLongLong();
        qint64 secret   = 0; // query.value("secret").toLongLong();
        qint64 dcId     = 1; // query.value("dcId").toLongLong();
        qint64 volumeId = 1; // query.value("volumeId").toLongLong();

        QString oldFilePath = query.value("localPath").toString();

        if (w == 0 || h == 0) {
            QImageReader reader(oldFilePath);
            if (reader.canRead()) {
                QSize size = reader.size();
                w = size.width();
                h = size.height();
            } else {
                w = 100;
                h = 100;
            }
        }


        insert.prepare("INSERT INTO PhotoSizes (pid, type, size, w, h, "
                "locationLocalId, locationSecret, locationDcId, locationVolumeId) "
                "VALUES (:pid, :type, :size, :w, :h, :localId, :secret, :dcId, :volumeId)");
        insert.bindValue(":pid", pid);
        insert.bindValue(":type", type);
        insert.bindValue(":size", size);
        insert.bindValue(":w", w);
        insert.bindValue(":h", h);
        insert.bindValue(":localId", localId);
        insert.bindValue(":secret", secret);
        insert.bindValue(":dcId", dcId);
        insert.bindValue(":volumeId", volumeId);

        if (!insert.exec()) {
            qCritical() << TAG << "failed to insert secret photo photosize" << insert.lastError();
            return;
        }

        QFile oldFile(oldFilePath);
        if (!oldFilePath.isEmpty() && oldFile.exists()) {
            QString newFilePath = QString("%1/%2_%3.jpg").arg(newPath).arg(1 /* volumeId */).arg(localId);

            bool hasCopied = QFile::copy(oldFilePath, newFilePath);
            if (DEBUG) qDebug() << TAG << "photo copying from" << oldFilePath << hasCopied;
            if (DEBUG) qDebug() << TAG << "photo copying   to" << newFilePath;
            if (!hasCopied) {
                qCritical() << TAG << "failed to copy photo attachment thumbnail";
            }
        }
    }
}

inline void UpgradeV2::copySecretVideo(qint64 peer, bool out, qint64 mediaId, QSqlDatabase &newDb) {
    QSqlQuery video(db);
    video.prepare("SELECT caption, mimeType, date, duration, width, height, size, userId, accessHash, localPath FROM mediaVideos WHERE id = :id");
    video.bindValue(":id", mediaId);
    if (!video.exec()) {
        qCritical() << TAG << "failed to get secret video" << video.lastError();
        return;
    }
    if (video.next()) {
        QSqlQuery newVideo(newDb);

        qint64 width = video.value("width").toLongLong();
        qint64 height = video.value("height").toLongLong();
        if (width == 0 || height == 0) {
            width = 100;
            height = 100;
        }
        qint64 accessHash = video.value("accessHash").toLongLong();
        if (accessHash == 0) accessHash = 1;

        newVideo.prepare("INSERT INTO Videos (id, dcId, caption, mimeType, date, duration, w, h, size, userId, accessHash, type) "
                "VALUES (:id, :dcId, :caption, :mimeType, :date, :duration, :w, :h, :size, :userId, :accessHash, :type)");
        newVideo.bindValue(":id", mediaId);
        newVideo.bindValue(":dcId", 1);
        newVideo.bindValue(":caption", video.value("caption").toString());
        newVideo.bindValue(":mimeType", video.value("mimeType").toString());
        newVideo.bindValue(":date", video.value("date").toLongLong());
        newVideo.bindValue(":duration", video.value("duration").toLongLong());
        newVideo.bindValue(":w", width);
        newVideo.bindValue(":h", height);
        newVideo.bindValue(":size", video.value("size").toLongLong());
        newVideo.bindValue(":userId", video.value("userId").toLongLong());
        newVideo.bindValue(":accessHash", accessHash);
        newVideo.bindValue(":type", MessageMedia::typeMessageMediaVideo);

        if (!newVideo.exec()) {
            qCritical() << TAG << "failed to insert secret video" << newVideo.lastError();
            return;
        }
    } else {
        qCritical() << TAG << "secret video not found";
        return;
    }

    QSqlQuery query(db);
    query.prepare("SELECT photoId, type, size, width, height, dcId, localId, secret, volumeId, localPath "
            "FROM photoSizes JOIN fileLocations ON photoSizes.fileLocationId = fileLocations.rowid "
            "WHERE photoId = :photoId");
    query.bindValue(":photoId", mediaId);
    if (!query.exec()) {
        qCritical() << TAG << "failed to get photo sizes for secret video" << query.lastError();
        return;
    }

    QString oldFilePath;
    if (out) {
        oldFilePath = video.value("localPath").toString();
    } else {
        oldFilePath = QString("%1/videos/%2.mp4").arg(cachePhonePath).arg(mediaId);
    }
    QString newPath = QString("%1/%2/downloads/%3").arg(cachePath).arg(phone).arg(peer);
    QString newThumbPath = QString("%1/thumb").arg(newPath);
    QString newFilePath = QString("%1/%2.mp4").arg(newPath).arg(mediaId);

    QDir dir;
    dir.mkpath(newThumbPath);
    bool hasCopied = QFile::copy(oldFilePath, newFilePath);
    if (DEBUG) qDebug() << TAG << "video copying from" << oldFilePath << hasCopied;
    if (DEBUG) qDebug() << TAG << "video copying   to" << newFilePath;
    if (!hasCopied) {
        qCritical() << TAG << "failed to copy secret video file";
    }

    QSqlQuery insert(newDb);
    for ( ; query.next(); localId++) {
        // Commented fields left intentionally to show missing metadata for secret chat attachments.
        qint64 pid      = query.value("photoId").toLongLong();
        QString type    = query.value("type").toString();
        qint64 size     = query.value("size").toLongLong();
        qint64 w        = query.value("width").toLongLong();
        qint64 h        = query.value("height").toLongLong();
        // qint64 localId  = query.value("localId").toLongLong();
        qint64 secret   = 0; // query.value("secret").toLongLong();
        qint64 dcId     = 1; // query.value("dcId").toLongLong();
        qint64 volumeId = 1; // query.value("volumeId").toLongLong();

        insert.prepare("INSERT INTO PhotoSizes (pid, type, size, w, h, "
                "locationLocalId, locationSecret, locationDcId, locationVolumeId) "
                "VALUES (:pid, :type, :size, :w, :h, :localId, :secret, :dcId, :volumeId)");
        insert.bindValue(":pid", pid);
        insert.bindValue(":type", type);
        insert.bindValue(":size", size);
        insert.bindValue(":w", w);
        insert.bindValue(":h", h);
        insert.bindValue(":localId", localId);
        insert.bindValue(":secret", secret);
        insert.bindValue(":dcId", dcId);
        insert.bindValue(":volumeId", volumeId);

        if (!insert.exec()) {
            qCritical() << TAG << "failed to insert secret video photosize" << insert.lastError();
            return;
        }

        const QString oldFilePath = query.value("localPath").toString();
        QFile oldFile(oldFilePath);
        if (!oldFilePath.isEmpty() && oldFile.exists()) {
            QString newThumbFilePath = QString("%1.jpg").arg(newFilePath);
            bool hasCopied = QFile::copy(oldFilePath, newThumbFilePath);
            if (DEBUG) qDebug() << TAG << "video thumb copying from" << oldFilePath << hasCopied;
            if (DEBUG) qDebug() << TAG << "video thumb copying   to" << newThumbFilePath;
            if (hasCopied) {
                oldFile.remove();
            } else {
                qCritical() << TAG << "failed to copy video attachment thumbnail";
            }
        }
    }
}

inline void UpgradeV2::copySecretDocument(qint64 peer, bool out, qint64 mediaId, QSqlDatabase &newDb) {
    QSqlQuery documents(db);
    documents.prepare("SELECT dcId, mimeType, date, fileName, size, accessHash, userId, localPath FROM mediaDocuments WHERE id = :id");
    documents.bindValue(":id", mediaId);
    if (!documents.exec()) {
        qCritical() << TAG << "failed to get secret document" << documents.lastError();
        return;
    }

    QString fileExtension;
    if (documents.next()) {
        QSqlRecord doc = documents.record();
        QSqlQuery newDocument(newDb);

        qint64 accessHash = doc.value("accessHash").toLongLong();
        if (accessHash == 0) accessHash = 1;

        QString originalFileName = doc.value("fileName").toString();
        int extensionPosition = originalFileName.indexOf(".");
        fileExtension = extensionPosition > 0 ? originalFileName.mid(extensionPosition + 1) : "";

        newDocument.prepare("INSERT INTO Documents (id, dcId, mimeType, date, fileName, size, accessHash, userId, type) "
                "VALUES (:id, :dcId, :mimeType, :date, :fileName, :size, :accessHash, :userId, :type)");
        newDocument.bindValue(":id", mediaId);
        newDocument.bindValue(":dcId", doc.value("dcId").toLongLong());
        newDocument.bindValue(":mimeType", doc.value("mimeType").toString());
        newDocument.bindValue(":date", doc.value("date").toLongLong());
        newDocument.bindValue(":fileName", originalFileName);
        newDocument.bindValue(":size", doc.value("size").toLongLong());
        newDocument.bindValue(":accessHash", accessHash);
        newDocument.bindValue(":userId", doc.value("userId").toLongLong());
        newDocument.bindValue(":type", MessageMedia::typeMessageMediaDocument);

        if (!newDocument.exec()) {
            qCritical() << TAG << "failed to insert secret document" << newDocument.lastError();
            return;
        }
    } else {
        qCritical() << TAG << "secret document not found";
        return;
    }

    QString fileName = fileExtension.isEmpty() ? QString::number(mediaId) : QString("%1.%2").arg(mediaId).arg(fileExtension);
    QString oldFilePath;
    if (out) {
        oldFilePath = documents.value("localPath").toString();
    } else {
        oldFilePath = QString("%1/documents/%3").arg(cachePhonePath).arg(fileName);
    }

    QString newPath = QString("%1/%2/downloads/%3").arg(cachePath).arg(phone).arg(peer);
    QString newFilePath = QString("%1/%2").arg(newPath).arg(fileName);

    QDir dir;
    dir.mkpath(newPath);

    bool hasCopied = QFile::copy(oldFilePath, newFilePath);
    if (DEBUG) qDebug() << TAG << "doc copying from" << oldFilePath << hasCopied;
    if (DEBUG) qDebug() << TAG << "doc copying   to" << newFilePath;
    if (!hasCopied) {
        qCritical() << TAG << "failed to copy secret document file";
    }
}

inline void UpgradeV2::copySecretMessage(qint64 peer, const QSqlRecord &message, QSqlDatabase &newDb) {
    QSqlQuery insert(newDb);
    insert.prepare("INSERT INTO Messages (id, toId, toPeerType, unread, fromId, out, date, fwdDate, fwdFromId, message, "
            "actionUserId, actionPhoto, actionType, mediaAudio, mediaPhoneNumber, mediaDocument, mediaGeo, mediaPhoto, mediaUserId, mediaVideo, mediaType) "
            "VALUES (:id, :toId, :toPeerType, :unread, :fromId, :out, :date, :fwdDate, :fwdFromId, :message, "
            "0, 0, :actionType, 0, 0, :mediaDocument, 0, :mediaPhoto, 0, :mediaVideo, :mediaType)");

    bool out = message.value("out").toInt();
    qint64 actionType = message.value("actionType").toLongLong();
    if (actionType == 0) {
        actionType = MessageAction::typeMessageActionEmpty;
    }
    qint64 date = message.value("date").toLongLong();
    qint64 mediaType = message.value("mediaType").toLongLong();
    qint64 mediaId = message.value("mediaId").toLongLong();

    insert.bindValue(":id", date); // Yes, that's right.
    insert.bindValue(":toId", peer);
    insert.bindValue(":fromId", message.value("fromId").toLongLong());
    insert.bindValue(":toPeerType", Peer::typePeerChat);
    insert.bindValue(":unread", message.value("unread").toLongLong());
    insert.bindValue(":out", out);
    insert.bindValue(":date", date);
    insert.bindValue(":fwdFromId", message.value("fwdFromId").toLongLong());
    insert.bindValue(":fwdDate", message.value("fwdDate").toLongLong());
    insert.bindValue(":message", message.value("message").toString());
    insert.bindValue(":actionType", actionType);
    insert.bindValue(":mediaPhoto", mediaType == MessageMedia::typeMessageMediaPhoto ? mediaId : 0);
    insert.bindValue(":mediaVideo", mediaType == MessageMedia::typeMessageMediaVideo ? mediaId : 0);
    insert.bindValue(":mediaDocument", mediaType == MessageMedia::typeMessageMediaDocument ? mediaId : 0);
    insert.bindValue(":mediaType", mediaType);

    if (!insert.exec()) {
        qCritical() << TAG << "failed to copy secret message" << insert.lastError();
        return;
    }

    if (mediaType == MessageMedia::typeMessageMediaPhoto) {
        copySecretPhoto(peer, out, mediaId, newDb);
    } else if (mediaType == MessageMedia::typeMessageMediaVideo) {
        copySecretVideo(peer, out, mediaId, newDb);
    } else if (mediaType == MessageMedia::typeMessageMediaDocument) {
        copySecretDocument(peer, out, mediaId, newDb);
    }
}

inline void UpgradeV2::copySecretMessages(qint64 peer, QSqlDatabase &newDb) {
    QSqlQuery messages(db);

    messages.prepare("SELECT id, toId, fromId, unread, out, date, fwdFromId, fwdDate, text as message, mediaId, mediaType, "
            "(SELECT type FROM messageActions WHERE messageId = id) AS actionType, "
            "(SELECT type FROM users WHERE users.id = toId) as toPeerType "
            "FROM messages WHERE dialogId = :dialogId");
    messages.bindValue(":dialogId", peer);

    if (!messages.exec()) {
        qCritical() << TAG << "failed to get messages for secret chat" << peer << messages.lastError();
        return;
    }

    while (messages.next()) {
        const QSqlRecord &message = messages.record();
        copySecretMessage(peer, message, newDb);
    }
}

inline void UpgradeV2::copySecretChat(const QSqlRecord &record, QSqlDatabase &newDb) {
    qint64 peer = record.value("id").toLongLong();
    qint64 peerType = Peer::typePeerUser;
    qint64 topMessage = 0L;
    qint64 unreadCount = record.value("unreadCount").toLongLong();
    bool encrypted = true;

    QSqlQuery insert(newDb);
    insert.prepare("INSERT INTO Dialogs (peer, peerType, topMessage, unreadCount, encrypted) "
            "VALUES (:peer, :peerType, :topMessage, :unreadCount, :encrypted)");
    insert.bindValue(":peer", peer);
    insert.bindValue(":peerType", peerType);
    insert.bindValue(":topMessage", topMessage);
    insert.bindValue(":unreadCount", unreadCount);
    insert.bindValue(":encrypted", encrypted);

    if (!insert.exec()) {
        qCritical() << TAG << "failed to insert secret chat to dialogs" << insert.lastError();
    } else {
        qDebug() << TAG << "inserted secret chat" << peer;
        copySecretMessages(peer, newDb);
    }
}

void UpgradeV2::copySecretChats() {
    db = QSqlDatabase::addDatabase("QSQLITE", "upgrade");
    db.setDatabaseName(databaseFilePath);
    db.open();

    QSqlDatabase newDb = QSqlDatabase::addDatabase("QSQLITE", "upgrade_new");
    newDb.setDatabaseName(newDatabasePath);
    newDb.open();

    QSqlQuery secretChats(db);
    secretChats.prepare("SELECT id, unreadCount FROM dialogs WHERE isSecret = 1");
    if (!secretChats.exec()) {
        qCritical() << TAG << "failed to get secret chats" << secretChats.lastError();
        return;
    }
    while (secretChats.next()) {
        const QSqlRecord &record = secretChats.record();
        copySecretChat(record, newDb);
    }
    db.close();
    newDb.close();
}

void UpgradeV2::insertProfile() {
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "upgrade_profiles");
    db.setDatabaseName(newProfilesPath);
    db.open();

    QSqlQuery query(db);
    // profile 'AA' name comes from Cutegram source. unrelevant as long as not used in UI.
    query.prepare("INSERT OR REPLACE INTO Profiles VALUES (:phone, 'AA', '', 0)");
    query.bindValue(":phone", phone);
    if (!query.exec()) {
        qCritical() << TAG << "failed to insert profile" << query.lastError();
    } else {
        qDebug() << TAG << "profile inserted";
    }
    db.close();
}

void UpgradeV2::deleteFiles() {
    QFile::remove(configFilePath);
}
