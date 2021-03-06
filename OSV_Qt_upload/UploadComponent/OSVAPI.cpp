#include "OSVAPI.h"
#include "metadata.h"
#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

OSVAPI::OSVAPI(QObject* parent)
    : QObject(parent)
    , m_uploadPaused(false)
{
    m_manager = new QNetworkAccessManager();

    // connect fail signal-slots
    connect(this, SIGNAL(NewSequenceFailed(PersistentSequence*, int)), this,
            SLOT(onNewSequenceFailed(PersistentSequence*, int)), Qt::QueuedConnection);
    connect(this, SIGNAL(SequenceFinishedFailed(PersistentSequence*, int)), this,
            SLOT(onSequenceFinishedFailed(PersistentSequence*, int)), Qt::QueuedConnection);
    connect(this, SIGNAL(NewPhotoFailed(PersistentSequence*, int, int)), this,
            SLOT(onNewPhotoFailed(PersistentSequence*, int, int)), Qt::QueuedConnection);
    connect(this, SIGNAL(NewVideoFailed(PersistentSequence*, int, int)), this,
            SLOT(onNewVideoFailed(PersistentSequence*, int, int)), Qt::QueuedConnection);
}

OSVAPI::~OSVAPI()
{
    delete m_manager;
}

QJsonObject OSVAPI::objectFromString(const QString& in)
{
    QJsonObject obj;

    QJsonDocument doc = QJsonDocument::fromJson(in.toUtf8());

    // check validity of the document
    if (!doc.isNull())
    {
        if (doc.isObject())
        {
            obj = doc.object();
        }
    }

    return obj;
}

void delay()
{
    const QTime dieTime(QTime::currentTime().addSecs(1));
    while (QTime::currentTime() < dieTime)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
}

void OSVAPI::requestNewSequence(PersistentSequence* sequence, const int sequenceIndex)
{
    if (m_uploadPaused)
    {
        return;
    }
    HTTPRequest* request = new HTTPRequest(NULL, m_manager);
    connect(request, SIGNAL(newBytesDifference(qint64)), this, SIGNAL(uploadProgress(qint64)));

    request->setHandlerFunc([=](QNetworkReply* reply) {
        bool sequenceFailed = false;
        if (reply && !m_uploadPaused)
        {
            QByteArray  data        = reply->readAll();
            QString     string_data = QString::fromLatin1(data.data());
            QJsonObject json        = objectFromString(string_data);
            qDebug() << string_data;
            QJsonObject   statusObj;
            OSVStatusCode statusCode = OSVStatusCode::STATUS_INCORRECT;
            if (!json.isEmpty())
            {
                statusObj  = json["status"].toObject();
                statusCode = (OSVStatusCode)statusObj["apiCode"].toString().toInt();
            }

            if (statusCode == OSVStatusCode::SUCCESS)
            {
                QJsonObject osvObj      = json["osv"].toObject();
                QJsonObject sequenceObj = osvObj["sequence"].toObject();

                sequence->read(sequenceObj);
                sequence->setSequenceStatus(SequenceStatus::BUSY);
                disconnect(request, SIGNAL(newBytesDifference(qint64)), this,
                           SIGNAL(uploadProgress(qint64)));
                emit sequenceCreated(sequenceIndex);
            }
            else if (statusCode == OSVStatusCode::BAD_LOGIN)
            {
                emit errorFound();
            }
            else
            {
                sequenceFailed = true;
            }
        }
        else
        {
            sequenceFailed = true;
        }

        if (sequenceFailed)
        {
            emit NewSequenceFailed(sequence, sequenceIndex);
        }

        // delete captured request
        reply->deleteLater();
        delete request;
    });

    QFile*     file(nullptr);
    QByteArray buffer(nullptr);
    if (!sequence->getMetadata()->getPath().isEmpty())
    {
        file = new QFile(sequence->getMetadata()->getPath());
        if (!file->open(QIODevice::ReadOnly))
        {
            qDebug() << "Can not open!";
        }
        else
        {
            buffer = file->read(file->size());
        }
    }

    const QString token(sequence->getToken());

    QHttpMultiPart* map = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart       filePart;

    double lat = sequence->getLat();
    double lng = sequence->getLng();

    bool emptyData = false;
    if ((buffer.isEmpty() && sequence->getVideos().size()) || sequence->getToken().isEmpty() ||
        !(lat && lng))
    {
        emptyData = true;
    }
    else
    {
        // TO DO : select corect mime-type -> + txt
        if (!buffer.isNull())
        {
            filePart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("application/x-gzip"));
            filePart.setHeader(
                QNetworkRequest::ContentDispositionHeader,
                QVariant("form-data; name=\"metaData\"; filename=\"" + file->fileName() + "\""));
            filePart.setBody(buffer);
            file->setParent(map);
            map->append(filePart);
            file->close();
        }

        // platform from metadata
        QHttpPart platformNamePart;
        platformNamePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                                   QVariant("form-data; name=\"platformName\""));
        platformNamePart.setBody(sequence->getMetadata()->getPlatformName().toLatin1());
        map->append(platformNamePart);

        QHttpPart platformVersionPart;
        platformVersionPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                                      QVariant("form-data; name=\"platformVersion\""));
        platformVersionPart.setBody(sequence->getMetadata()->getPlatformVersion().toLatin1());
        map->append(platformVersionPart);

        QHttpPart clientTokenPart;
        clientTokenPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                                  QVariant("form-data; name=\"access_token\""));
        clientTokenPart.setBody(sequence->getToken().toLatin1());
        map->append(clientTokenPart);

        QString   coordinate = QString::number(lat) + "," + QString::number(lng);
        QHttpPart coordinatePart;
        coordinatePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                                 QVariant("form-data; name=\"currentCoordinate\""));
        coordinatePart.setBody(coordinate.toLatin1());
        map->append(coordinatePart);

        QHttpPart     uploadSourcePart;
        const QString uploadSource("Qt tool");
        uploadSourcePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                                   QVariant("form-data; name=\"uploadSource\""));
        uploadSourcePart.setBody(uploadSource.toLatin1());
        map->append(uploadSourcePart);
    }

    const QString url(kProtocol + kBaseProductionUrl + kVersion + kCommandSequence);
    qDebug() << "Request URL: " << url;
    qDebug() << "Metadata: " << sequence->getMetadata()->getPath();
    if (!emptyData)
    {
        request->post(url, map);
    }
    else
    {
        // POP UP
        qDebug() << "POP UP";
    }
}

void OSVAPI::onNewSequenceFailed(PersistentSequence* sequence, const int sequenceIndex)
{
    qDebug() << "New Sequence Failed!";
    sequence->setSequenceStatus(SequenceStatus::FAILED);

    delay();
    requestNewSequence(sequence, sequenceIndex);
}

void OSVAPI::requestSequenceFinished(
    PersistentSequence* sequence, const int sequenceIndex)  // sequenceId, externalUserId, userType
{
    if (m_uploadPaused)
    {
        return;
    }

    HTTPRequest* request = new HTTPRequest(NULL, m_manager);
    connect(request, SIGNAL(newBytesDifference(qint64)), this, SIGNAL(uploadProgress(qint64)));

    request->setHandlerFunc([=](QNetworkReply* reply) {
        bool sequenceFailed = false;
        if (reply && !m_uploadPaused)
        {
            QByteArray data        = reply->readAll();
            QString    string_data = QString::fromLatin1(data.data());
            qDebug() << string_data;
            QJsonObject json = objectFromString(string_data);

            QJsonObject   statusObj;
            OSVStatusCode statusCode = OSVStatusCode::STATUS_INCORRECT;
            if (!json.isEmpty())
            {
                statusObj  = json["status"].toObject();
                statusCode = (OSVStatusCode)statusObj["apiCode"].toString().toInt();
            }

            if (statusCode == OSVStatusCode::SUCCESS &&
                sequence->getSequenceStatus() != SequenceStatus::SUCCESS)
            {
                sequence->setSequenceStatus(SequenceStatus::SUCCESS);
                disconnect(request, SIGNAL(newBytesDifference(qint64)), this,
                           SIGNAL(uploadProgress(qint64)));
                emit SequenceFinished(sequenceIndex);
            }
            else
            {
                qDebug() << "Incorrect sequence finished!";
                sequenceFailed = true;
            }
        }
        else
        {
            qDebug() << "Bad reply! ( sequence finished ) ";
            sequenceFailed = true;
        }

        if (sequenceFailed)
        {
            emit SequenceFinishedFailed(sequence, sequenceIndex);
        }

        reply->deleteLater();
        delete request;
    });

    QMap<QString, QString> map = QMap<QString, QString>();
    bool emptyData = false;

    if (sequence->sequenceId() < 0)
    {
        emptyData = true;
    }
    else
    {
        map.insert("sequenceId", QString::number(sequence->sequenceId()));
        map.insert("access_token", sequence->getToken());
    }

    const QString url(kProtocol + kBaseProductionUrl + kVersion + kCommandSequenceFinished);
    qDebug() << "Request URL: " << url;
    if (!emptyData)
    {
        request->post(url, map);
    }
    else
    {
        // POP UP
        qDebug() << "POP UP";
    }
}

void OSVAPI::onSequenceFinishedFailed(PersistentSequence* sequence, const int sequenceIndex)
{
    qDebug() << "Sequence Finish Failed! Bad Reply!";
    sequence->setSequenceStatus(SequenceStatus::FAILED_FINISH);
    delay();
    requestSequenceFinished(sequence, sequenceIndex);
}

void OSVAPI::requestNewPhoto(PersistentSequence* sequence, const int sequenceIndex,
                             const int photoIndex)
{
    const QList<Photo*> photoList = sequence->getPhotos();

    if (m_uploadPaused || photoIndex >= photoList.count())
    {
        return;
    }
    Photo*       currentPhoto = photoList.at(photoIndex);
    HTTPRequest* request      = new HTTPRequest(NULL, m_manager);
    connect(request, SIGNAL(newBytesDifference(qint64)), this, SIGNAL(uploadProgress(qint64)));

    request->setHandlerFunc([=](QNetworkReply* reply) {
        currentPhoto->setStatus(FileStatus::BUSY);

        OSVStatusCode statusCode;
        bool          sequenceFailed = false;
        if (reply && !m_uploadPaused)
        {
            QByteArray data        = reply->readAll();
            QString    string_data = QString::fromLatin1(data.data());

            QJsonObject json = objectFromString(string_data);
            qDebug() << string_data;
            QJsonObject statusObj;
            statusCode = OSVStatusCode::STATUS_INCORRECT;
            if (!json.isEmpty())
            {
                statusObj  = json["status"].toObject();
                statusCode = (OSVStatusCode)statusObj["apiCode"].toString().toInt();
            }
            if (statusCode == OSVStatusCode::SUCCESS)
            {
                qDebug() << "Succes, photo index: " << photoIndex;
                if (currentPhoto)
                {
                    currentPhoto->setStatus(FileStatus::DONE);
                }
                disconnect(request, SIGNAL(newBytesDifference(qint64)), this,
                           SIGNAL(uploadProgress(qint64)));
                emit photoUploaded(sequenceIndex, photoIndex);
            }
            else if (statusCode == OSVStatusCode::DUPLICATE)
            {
                currentPhoto->setStatus(FileStatus::DONE);
                emit photoUploaded(sequenceIndex, photoIndex);
            }
            else
            {
                sequenceFailed = true;
            }
        }
        else
        {
            sequenceFailed = true;
            qDebug() << "Bad reply! ( New photo ) ";
        }
        if (sequenceFailed)
        {
            emit NewPhotoFailed(sequence, sequenceIndex, photoIndex);
        }

        reply->deleteLater();
        delete request;
    });

    QFile*     imageFile(nullptr);
    QByteArray buffer(nullptr);

    imageFile = new QFile(currentPhoto->getPath());
    if (!imageFile->open(QIODevice::ReadOnly))
    {
        qDebug() << "Can not open image!";
    }
    buffer = imageFile->read(imageFile->size());

    QHttpMultiPart* map = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    double lat = currentPhoto->getLat();
    double lng = currentPhoto->getLng();

    bool isEmpty    = false;
    int  sequenceId = sequence->sequenceId();

    if (buffer.isEmpty() || sequenceId < 0 || photoIndex < 0 || !(lat && lng))
    {
        isEmpty = true;
    }
    else
    {
        QHttpPart imagePart;
        imagePart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("image/jpeg"));
        imagePart.setHeader(
            QNetworkRequest::ContentDispositionHeader,
            QVariant("form-data; name=\"photo\"; filename=\"" + imageFile->fileName() + "\""));
        imagePart.setBody(buffer);
        imageFile->setParent(map);
        map->append(imagePart);
        imageFile->close();

        QHttpPart seqIdPart;
        seqIdPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                            QVariant("form-data; name=\"sequenceId\""));
        seqIdPart.setBody(QString::number(sequenceId).toLatin1());
        map->append(seqIdPart);

        QHttpPart seqIndexPart;
        seqIndexPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                               QVariant("form-data; name=\"sequenceIndex\""));
        seqIndexPart.setBody(QString::number(photoIndex).toLatin1());
        map->append(seqIndexPart);

        QHttpPart coordinatePart;
        coordinatePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                                 QVariant("form-data; name=\"coordinate\""));
        coordinatePart.setBody((QString::number(lat) + "," + QString::number(lng)).toLatin1());
        map->append(coordinatePart);

        QHttpPart accessTokenPart;
        accessTokenPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                                  QVariant("form-data; name=\"access_token\""));
        accessTokenPart.setBody(sequence->getToken().toLatin1());
        map->append(accessTokenPart);
    }
    const QString url(kProtocol + kBaseProductionUrl + kVersion + kCommandPhoto);
    qDebug() << "Request URL : " << url << " --> PhotoIndex : " << photoIndex
             << " | FileName: " << QFileInfo(currentPhoto->getPath()).baseName()
             << " | Coord : " << lat << " - " << lng;

    if (!isEmpty)
    {
        currentPhoto->setStatus(FileStatus::BUSY);
        request->post(url, map);
    }
}

void OSVAPI::onNewPhotoFailed(PersistentSequence* sequence, const int sequenceIndex,
                              const int photoIndex)
{
    qDebug() << "New Photo Failed! Bad Reply! " << photoIndex;
    delay();
    requestNewPhoto(sequence, sequenceIndex, photoIndex);
}

void OSVAPI::requestNewVideo(PersistentSequence* sequence, const int sequenceIndex,
                             const int videoIndex)
{
    const QList<Video*> videoList = sequence->getVideos();

    if (m_uploadPaused || videoIndex >= videoList.count())
    {
        return;
    }
    Video*       currentVideo = videoList.at(videoIndex);
    HTTPRequest* request      = new HTTPRequest(NULL, m_manager);
    connect(request, SIGNAL(newBytesDifference(qint64)), this, SIGNAL(uploadProgress(qint64)));

    request->setHandlerFunc([=](QNetworkReply* reply) {
        currentVideo->setStatus(FileStatus::BUSY);
        OSVStatusCode statusCode;
        bool          sequenceFailed = false;
        if (reply && !m_uploadPaused)
        {
            QByteArray  data        = reply->readAll();
            QString     string_data = QString::fromLatin1(data.data());
            QJsonObject json        = objectFromString(string_data);
            QJsonObject statusObj;
            statusCode = OSVStatusCode::STATUS_INCORRECT;
            if (!json.isEmpty())
            {
                statusObj  = json["status"].toObject();
                statusCode = (OSVStatusCode)statusObj["apiCode"].toString().toInt();
            }

            if (statusCode == OSVStatusCode::SUCCESS)
            {
                qDebug() << "Success, video index: " << videoIndex
                         << " | videoPath: " << currentVideo->getPath();
                disconnect(request, SIGNAL(newBytesDifference(qint64)), this,
                           SIGNAL(uploadProgress(qint64)));
                if (videoIndex < videoList.count())
                {
                    currentVideo->setStatus(FileStatus::DONE);
                }
                emit videoUploaded(sequenceIndex, videoIndex);
            }
            else if (statusCode == OSVStatusCode::DUPLICATE)
            {
                currentVideo->setStatus(FileStatus::DONE);
                emit videoUploaded(sequenceIndex, videoIndex);
            }
            else
            {
                sequenceFailed = true;
            }
        }
        else
        {
            sequenceFailed = true;
        }

        if (sequenceFailed)
        {
            reply->deleteLater();
            emit NewVideoFailed(sequence, sequenceIndex, videoIndex);
        }

        reply->deleteLater();
        delete request;
    });

    QFile*     videoFile(nullptr);
    QByteArray buffer(nullptr);

    videoFile = new QFile(currentVideo->getPath());
    if (!videoFile->open(QIODevice::ReadOnly))
    {
        qDebug() << "Can not open image!";
    }
    else
    {
        buffer = videoFile->read(videoFile->size());
    }

    QHttpMultiPart* map = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    bool isEmpty    = false;
    int  sequenceId = sequence->sequenceId();

    if (buffer.isEmpty() || sequenceId < 0 || videoIndex < 0)
    {
        isEmpty = true;
    }
    else
    {
        QHttpPart videoPart;
        videoPart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("video/mp4"));
        videoPart.setHeader(
            QNetworkRequest::ContentDispositionHeader,
            QVariant("form-data; name=\"video\"; filename=\"" + videoFile->fileName() + "\""));
        videoPart.setBody(buffer);
        videoFile->setParent(map);
        map->append(videoPart);
        videoFile->close();

        QHttpPart seqIdPart;
        seqIdPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                            QVariant("form-data; name=\"sequenceId\""));
        seqIdPart.setBody(QString::number(sequenceId).toLatin1());
        map->append(seqIdPart);

        QHttpPart seqIndexPart;
        seqIndexPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                               QVariant("form-data; name=\"sequenceIndex\""));
        seqIndexPart.setBody(QString::number(videoIndex).toLatin1());
        map->append(seqIndexPart);

        QHttpPart accessTokenPart;
        accessTokenPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                                  QVariant("form-data; name=\"access_token\""));
        accessTokenPart.setBody(sequence->getToken().toLatin1());
        map->append(accessTokenPart);
    }

    const QString url(kProtocol + kBaseProductionUrl + kVersion + kCommandVideo);
    qDebug() << "Request URL : " << url << " SequenceIndex: " << videoIndex
             << " | Video fileName: " << QFileInfo(currentVideo->getPath()).baseName();
    if (!isEmpty)
    {
        currentVideo->setStatus(FileStatus::BUSY);
        request->post(url, map);
    }

    delete videoFile;
}

void OSVAPI::onNewVideoFailed(PersistentSequence* sequence, const int sequenceIndex,
                              const int videoIndex)
{
    qDebug() << "New video Failed! Bad Reply! " << videoIndex;
    delay();
    requestNewVideo(sequence, sequenceIndex, videoIndex);
}

void OSVAPI::pauseUpload()
{
    m_uploadPaused = true;
}

void OSVAPI::resumeUpload()
{
    m_uploadPaused = false;
}
