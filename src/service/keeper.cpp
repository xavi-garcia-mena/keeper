/*
 * Copyright (C) 2016 Canonical, Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *     Xavi Garcia <xavi.garcia.mena@canonical.com>
 *     Charles Kerr <charles.kerr@canonical.com>
 */

#include "app-const.h" // DEKKO_APP_ID
#include "helper/backup-helper.h"
#include "helper/metadata.h"
#include "service/metadata-provider.h"
#include "service/keeper.h"
#include "storage-framework/storage_framework_client.h"

#include <QDebug>
#include <QDBusMessage>
#include <QDBusConnection>
#include <QSharedPointer>
#include <QScopedPointer>
#include <QVariant>
#include <QVector>

#include <uuid/uuid.h>


class KeeperPrivate
{
public:

    Keeper * const q_ptr;
    QSharedPointer<HelperRegistry> helper_registry_;
    QSharedPointer<MetadataProvider> backup_choices_;
    QSharedPointer<MetadataProvider> restore_choices_;
    QScopedPointer<BackupHelper> backup_helper_;
    QScopedPointer<StorageFrameworkClient> storage_;
    QVector<Metadata> cached_backup_choices_;
    QVector<Metadata> cached_restore_choices_;
    QStringList remaining_tasks_;

    KeeperPrivate(Keeper* keeper,
                  const QSharedPointer<HelperRegistry>& helper_registry,
                  const QSharedPointer<MetadataProvider>& backup_choices,
                  const QSharedPointer<MetadataProvider>& restore_choices)
        : q_ptr(keeper)
        , helper_registry_(helper_registry)
        , backup_choices_(backup_choices)
        , restore_choices_(restore_choices)
        , backup_helper_(new BackupHelper(DEKKO_APP_ID))
        , storage_(new StorageFrameworkClient())
        , cached_backup_choices_()
        , cached_restore_choices_()
        , remaining_tasks_()
    {
        // listen for backup helper state changes
        QObject::connect(backup_helper_.data(), &Helper::state_changed,
            std::bind(&KeeperPrivate::on_backup_helper_state_changed, this, std::placeholders::_1)
        );
    }

    ~KeeperPrivate() =default;

    Q_DISABLE_COPY(KeeperPrivate)

    void start_task(QString const &uuid)
    {
        qDebug() << "Starting task: " << uuid;
        auto metadata = get_uuid_metadata(cached_backup_choices_, uuid);
        if (metadata.uuid() == uuid)
        {
            qDebug() << "Task is a backup task";

            const auto urls = helper_registry_->get_backup_helper_urls(metadata);
            if (urls.isEmpty())
            {
                // TODO Report error to user
                qWarning() << "ERROR: uuid: " << uuid << " has no url";
                return;
            }

            backup_helper_->start(urls);
        }
    }

    void start_tasks(QStringList const& tasks)
    {
        remaining_tasks_ = tasks;
        start_remaining_tasks();
    }

    void start_remaining_tasks()
    {
        if (remaining_tasks_.size())
        {
            auto task = remaining_tasks_.takeFirst();
            start_task(task);
        }
    }

    void clear_remaining_taks()
    {
        remaining_tasks_.clear();
    }

private:

    void on_backup_helper_state_changed(Helper::State state)
    {
        switch (state)
        {
            case Helper::State::NOT_STARTED:
                break;

            case Helper::State::STARTED:
                qDebug() << "Backup helper started";
                break;

            case Helper::State::CANCELLED:
                qDebug() << "Backup helper cancelled... closing the socket.";
                storage_->closeUploader();
                break;
            case Helper::State::FAILED:
                qDebug() << "Backup helper failed... closing the socket.";
                storage_->closeUploader();
                break;
            case Helper::State::COMPLETE:
                qDebug() << "Backup helper finished... closing the socket.";
                storage_->closeUploader();
                break;
        }
    }

    Metadata get_uuid_metadata(QVector<Metadata> const &metadata, QString const & uuid)
    {
        for (auto item : metadata)
        {
            if (item.uuid() == uuid)
            {
                return item;
            }
        }
        return Metadata();
    }
};


Keeper::Keeper(const QSharedPointer<HelperRegistry>& helper_registry,
               const QSharedPointer<MetadataProvider>& backup_choices,
               const QSharedPointer<MetadataProvider>& restore_choices,
               QObject* parent)
    : QObject(parent)
    , d_ptr(new KeeperPrivate(this, helper_registry, backup_choices, restore_choices))
{
    Q_D(Keeper);
    QObject::connect(d->storage_.data(), &StorageFrameworkClient::uploaderClosed, this, &Keeper::socketClosed);
}

Keeper::~Keeper() = default;

void Keeper::start_tasks(QStringList const & keys)
{
    Q_D(Keeper);

    d->start_tasks(keys);
}

QDBusUnixFileDescriptor
Keeper::StartBackup(QDBusConnection bus, const QDBusMessage& msg, quint64 n_bytes)
{
    Q_D(Keeper);

    qDebug("Helper::StartBackup(n_bytes=%zu)", size_t(n_bytes));

    // the next time we get a socket from storage-framework, return it to our caller
    auto on_socket_ready = [bus,msg,n_bytes,this,d](std::shared_ptr<QLocalSocket> const &sf_socket)
    {
        if (sf_socket)
        {
            qDebug("getNewFileForBackup() returned socket %d", int(sf_socket->socketDescriptor()));
            qDebug("calling helper.set_storage_framework_socket(n_bytes=%zu socket=%d)", size_t(n_bytes), int(sf_socket->socketDescriptor()));
            d->backup_helper_->set_expected_size(n_bytes);
            d->backup_helper_->set_storage_framework_socket(sf_socket);
        }
        auto reply = msg.createReply();
        reply << QVariant::fromValue(QDBusUnixFileDescriptor(d->backup_helper_->get_helper_socket()));
        bus.send(reply);
    };
    // cppcheck-suppress deallocuse
    QObject::connect(d->storage_.data(), &StorageFrameworkClient::socketReady, on_socket_ready);

    // ask storage framework for a new socket
    d->storage_->getNewFileForBackup(n_bytes);

    // tell the caller that we'll be responding async
    msg.setDelayedReply(true);
    return QDBusUnixFileDescriptor(0);
}

// FOR TESTING PURPOSES ONLY
// we should finish when the helper finishes
void Keeper::finish()
{
    Q_D(Keeper);

    qDebug() << "Closing the socket-------";

    d->storage_->closeUploader();
}

void Keeper::socketClosed(std::shared_ptr<unity::storage::qt::client::File> const & file_created)
{
    Q_D(Keeper);

    qDebug() << "The storage framework socket was closed";
    if (d->backup_helper_->state() == Helper::State::COMPLETE)
    {
        if (d->storage_->removeTmpSuffix(file_created))
        {
            // the helper finished successfully, process more tasks
            d->start_remaining_tasks();
        }
    }
    else
    {
        d->storage_->deleteFile(file_created);
        // error or cancel case... clear all remaining tasks
        d->clear_remaining_taks();
    }
}

QVector<Metadata>
Keeper::get_backup_choices()
{
    Q_D(Keeper);

    if (!d->cached_backup_choices_.size())
    {
        d->cached_backup_choices_ = d->backup_choices_->get_backups();
    }
    return d->cached_backup_choices_;
}

QVector<Metadata>
Keeper::get_restore_choices()
{
    Q_D(Keeper);

    if (!d->cached_restore_choices_.size())
    {
        d->cached_restore_choices_ = d->restore_choices_->get_backups();
    }
    return d->cached_restore_choices_;
}
