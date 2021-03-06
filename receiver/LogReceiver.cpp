#include "LogReceiver.h"

#include <qnetworkproxy.h>
#include <qtcpsocket.h>
#include <qdatetime.h>
#include <qcoreapplication.h>
#include <qdebug.h>

LogReceiver::LogReceiver() 
    : QObject(nullptr)
    , validator(nullptr)
    , getNewThreadHandler(nullptr)
    , waitForClose(false)
{
    tcpServer = new QTcpServer(this);
    tcpServer->setProxy(QNetworkProxy::NoProxy);

    connect(tcpServer, &QTcpServer::newConnection, this, &LogReceiver::acceptNewConnection);

    connect(qApp, &QCoreApplication::aboutToQuit, [&] {
        waitForClose = true;
        tcpServer->close();
    });
}

LogReceiver::~LogReceiver() {
}

void LogReceiver::listen(const QHostAddress& address, int port) {
    if (tcpServer->isListening()) {
        tcpServer->close();
    }
    tcpServer->listen(address, port);
}

void LogReceiver::reselectProcess(const ConnectData& data, bool death) {
    auto clientData = getClient(data, death);
    if (clientData != nullptr) {
        for (const auto& threadName : clientData->savedThreads) {
            getNewThreadHandler(*clientData, threadName);
        }
        for (const auto& log : clientData->data) {
            if (validator != nullptr && validator(*clientData, log)) {
                emit clientGotLog(log);
            }
        }
    }
}

void LogReceiver::reloadProcessLog(const ConnectData& data, bool death) {
    auto clientData = getClient(data, death);
    if (clientData != nullptr) {
        for (const auto& log : clientData->data) {
            if (validator != nullptr && validator(*clientData, log)) {
                emit clientGotLog(log);
            }
        }
    }
}

void LogReceiver::clearLog(const ConnectData& data, bool death) {
    auto clientData = getClient(data, death);
    if (clientData != nullptr) {
        clientData->data.clear();
    }
}

ClientData* LogReceiver::getClient(const ConnectData& data, bool death) {
    ClientData* clientData = nullptr;
    if (death) {
        clientData = &deathProcess[data];
    } else {
        for (auto& d : clients) {
            if (d.info == data) {
                clientData = &d;
                break;
            }
        }
    }
    return clientData;
}

void LogReceiver::addNewClient(QTcpSocket* client) {
    clients.insert(client, {});
    connect(client, qOverload<QAbstractSocket::SocketError>(&QAbstractSocket::error), [&, client] (QAbstractSocket::SocketError e) {
        QString errStr;
        QDebug(&errStr) << e;

        auto clientData = clients.value(client);
        LogData data;
        data.threadName = clientData.info.processName;
        data.threadId = clientData.info.processId;
        data.level = LEVEL_ERROR;
        data.time = QDateTime::currentMSecsSinceEpoch();
        data.log = QStringLiteral("进程连接已断开！\n");
        data.tag = errStr;
        clients[client].data << data;
        if (validator != nullptr && validator(clientData, data)) {
            emit clientGotLog(data);
        }
    });

    connect(client, &QAbstractSocket::disconnected, [&, client] {
        auto data = clients.take(client);
        emit clientClosed(data.info);
        client->deleteLater();
        deathProcess.insert(data.info, data);

        if (waitForClose && clients.isEmpty()) {
            delete this;
        }
    });

    connect(client, &QAbstractSocket::readyRead, [&, client] {
        auto data = client->readAll();
        auto& clientData = clients[client];
        if (clientData.info.clientReady) {
            clientData.receiveBuffer.append(data);
            handleBuffer(clientData);
        } else {
            tryProcessInfo(data, clientData);
            if (clientData.info.clientReady) {
                client->write("ready");
            }
        }
    });

    client->write("who");
}

void LogReceiver::handleBuffer(ClientData& clientData) {
    int index = clientData.receiveBuffer.indexOf(',');
    while (index != -1) {
        auto subStr = clientData.receiveBuffer.mid(0, index);
        LogData data;
        data.fromTransData(subStr);
        if (!data.log.isEmpty()) {
            clientData.data << data;
            if (!data.threadName.isEmpty() && !clientData.savedThreads.contains(data.threadName)) {
                clientData.savedThreads << data.threadName;
                if (getNewThreadHandler != nullptr) {
                    getNewThreadHandler(clientData, data.threadName);
                 }
            }
            if (validator != nullptr && validator(clientData, data)) {
                emit clientGotLog(data);
            }
        }
        clientData.receiveBuffer = clientData.receiveBuffer.mid(index + 1);
        index = clientData.receiveBuffer.indexOf(',');
    }
}

void LogReceiver::tryProcessInfo(const QByteArray& data, ClientData& clientData) {
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isNull()) {
        auto obj = doc.object();
        if (obj.contains("processName")) {
            clientData.info.processName = obj.value("processName").toString();
            if (obj.contains("processId")) {
                clientData.info.processId = obj.value("processId").toVariant().toLongLong();
                clientData.info.clientReady = true;
                emit clientConnneted(clientData.info);
            }
        }
    }
}

void LogReceiver::acceptNewConnection() {
    while (tcpServer->hasPendingConnections()) {
        auto client = tcpServer->nextPendingConnection();
        addNewClient(client);
    }
}
