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
 *   Xavi Garcia <xavi.garcia.mena@canonical.com>
 *   Charles Kerr <charles.kerr@canonical.com>
 */

#include "storage-framework/storage_framework_client.h"
#include "storage-framework/sf-downloader.h"
#include "storage-framework/sf-uploader.h"

#include <QDateTime>
#include <QVector>
#include <QString>

namespace sf = unity::storage::qt::client;

/***
****
***/

const QString StorageFrameworkClient::KEEPER_FOLDER = QStringLiteral("Ubuntu-Backups");

StorageFrameworkClient::StorageFrameworkClient(QObject *parent)
    : QObject(parent)
    , runtime_(sf::Runtime::create())
{
}

StorageFrameworkClient::~StorageFrameworkClient() = default;

/***
****
***/

sf::Account::SPtr
StorageFrameworkClient::choose(QVector<sf::Account::SPtr> const& choices) const
{
    sf::Account::SPtr ret;

    qDebug() << "choosing from" << choices.size() << "accounts";
    if (choices.empty())
    {
        qWarning() << "no storage-framework accounts to pick from";
        last_error_ = keeper::Error::NO_REMOTE_ACCOUNTS;
    }
    else // for now just pick the first one. FIXME
    {
        for (auto& account : choices)
        {
            qDebug() << "Storage framework account found: [" << get_account_id(account) << "]";
        }
        if (storage_id_.isEmpty())
        {
            // FIXME use the default one, taking it from where it is defined
            ret = choices.front();
        }
        else
        {
            for (auto& account : choices)
            {
                if (get_account_id(account) == storage_id_)
                {
                    ret = account;
                    break;
                }
            }
            if (!ret)
            {
                qWarning() << "Storage framework account [" << storage_id_ << "] was not found";
                last_error_ = keeper::Error::ACCOUNT_NOT_FOUND;
            }
        }
    }

    return ret;
}

sf::Root::SPtr
StorageFrameworkClient::choose(QVector<sf::Root::SPtr> const& choices) const
{
    sf::Root::SPtr ret;

    if (last_error_ != keeper::Error::ACCOUNT_NOT_FOUND)
    {
        qDebug() << "choosing from" << choices.size() << "roots";
        if (choices.empty())
        {
            qWarning() << "no storage-framework roots to pick from";
            last_error_ = keeper::Error::NO_REMOTE_ROOTS;
        }
        else // for now just pick the first one. FIXME
        {
            ret = choices.front();
        }
    }

    return ret;
}

/***
****
***/

void
StorageFrameworkClient::add_accounts_task(std::function<void(QVector<sf::Account::SPtr> const&)> task)
{
    connection_helper_.connect_future(runtime_->accounts(), task);
}

void
StorageFrameworkClient::add_roots_task(std::function<void(QVector<sf::Root::SPtr> const&)> task)
{
    add_accounts_task([this, task](QVector<sf::Account::SPtr> const& accounts)
    {
        auto account = choose(accounts);
        if (account)
            connection_helper_.connect_future(account->roots(), task);
        else
        {
            QVector<sf::Root::SPtr> no_accounts;
            task(no_accounts);
        }
    });
}

void StorageFrameworkClient::set_storage(QString const & storage)
{
    storage_id_ = storage;
}

QFuture<std::shared_ptr<Uploader>>
StorageFrameworkClient::get_new_uploader(int64_t n_bytes, QString const & dir_name, QString const & file_name)
{
    clear_last_error();

    QFutureInterface<std::shared_ptr<Uploader>> fi;

    add_roots_task([this, fi, n_bytes, dir_name, file_name](QVector<sf::Root::SPtr> const& roots)
    {
        auto root = choose(roots);
        if (root)
        {
            connection_helper_.connect_future(
                get_keeper_folder(root, dir_name, true),
                std::function<void(sf::Folder::SPtr const&)>{
                    [this, fi, n_bytes, file_name,root](sf::Folder::SPtr const& keeper_folder){
                        if (!keeper_folder)
                        {
                            qWarning() << "Error creating keeper root folder";
                            std::shared_ptr<Uploader> ret;
                            QFutureInterface<decltype(ret)> qfi(fi);
                            qfi.reportResult(ret);
                            qfi.reportFinished();
                        }
                        else
                        {
                            connection_helper_.connect_future(
                                keeper_folder->create_file(file_name, n_bytes),
                                std::function<void(std::shared_ptr<sf::Uploader> const&)>{
                                    [this, fi, keeper_folder](std::shared_ptr<sf::Uploader> const& sf_uploader){
                                        qDebug() << "keeper_root->create_file() finished";
                                        std::shared_ptr<Uploader> ret;
                                        if (sf_uploader)
                                        {
                                            ret.reset(
                                                new StorageFrameworkUploader(sf_uploader, this),
                                                [](Uploader* u){u->deleteLater();}
                                            );
                                        }
                                        else
                                        {
                                            last_error_ = keeper::Error::CREATING_REMOTE_FILE;
                                        }
                                        QFutureInterface<decltype(ret)> qfi(fi);
                                        qfi.reportResult(ret);
                                        qfi.reportFinished();
                                    }
                                }
                            );
                        }
                    }
                }
            );
        }
        else
        {
            std::shared_ptr<Uploader> ret;
            QFutureInterface<decltype(ret)> qfi(fi);
            qfi.reportResult(ret);
            qfi.reportFinished();
        }
    });

    return fi.future();
}

QFuture<std::shared_ptr<Downloader>>
StorageFrameworkClient::get_new_downloader(QString const & dir_name, QString const & file_name)
{
    clear_last_error();

    QFutureInterface<std::shared_ptr<Downloader>> fi;

    add_roots_task([this, fi, dir_name, file_name](QVector<sf::Root::SPtr> const& roots)
    {
        auto root = choose(roots);
        if (root)
        {
            connection_helper_.connect_future(
                get_keeper_folder(root, dir_name, false),
                std::function<void(sf::Folder::SPtr const&)>{
                    [this, fi, file_name, root](sf::Folder::SPtr const& keeper_root){
                        if (!keeper_root)
                        {
                            qWarning() << "Error accessing keeper root folder";
                            std::shared_ptr<Downloader> ret;
                            QFutureInterface<decltype(ret)> qfi(fi);
                            qfi.reportResult(ret);
                            qfi.reportFinished();
                        }
                        else
                        {
                            qDebug() << "We found the storage-framework root folder" << keeper_root->name();
                            connection_helper_.connect_future(
                                get_storage_framework_file(keeper_root, file_name),
                                std::function<void(sf::File::SPtr const&)>{
                                    [this, fi, root, keeper_root](sf::File::SPtr const& sf_file){
                                        if (sf_file) {
                                            connection_helper_.connect_future(
                                                sf_file->create_downloader(),
                                                std::function<void(sf::Downloader::SPtr const&)>{
                                                    [this, fi, sf_file, keeper_root, root](sf::Downloader::SPtr const& sf_downloader){
                                                        std::shared_ptr<Downloader> ret;
                                                        if (sf_downloader)
                                                        {
                                                            ret.reset(
                                                                new StorageFrameworkDownloader(sf_downloader, sf_file->size(), this),
                                                                [](Downloader* d){d->deleteLater();}
                                                            );
                                                        }
                                                        else
                                                        {
                                                            last_error_ = keeper::Error::READING_REMOTE_FILE;
                                                        }
                                                        QFutureInterface<decltype(ret)> qfi(fi);
                                                        qfi.reportResult(ret);
                                                        qfi.reportFinished();
                                                    }
                                                }
                                            );
                                        } else {
                                            last_error_ = keeper::Error::READING_REMOTE_FILE;
                                            std::shared_ptr<Downloader> ret_null;
                                            QFutureInterface<decltype(ret_null)> qfi(fi);
                                            qfi.reportResult(ret_null);
                                            qfi.reportFinished();
                                        }
                                    }
                                }
                            );
                        }
                    }
                }
            );
        }
        else
        {
            std::shared_ptr<Downloader> ret;
            QFutureInterface<decltype(ret)> qfi(fi);
            qfi.reportResult(ret);
            qfi.reportFinished();
        }
    });

    return fi.future();
}

QFuture<QVector<QString>>
StorageFrameworkClient::get_keeper_dirs()
{
    clear_last_error();

    QFutureInterface<QVector<QString>> fi;

    add_roots_task([this, fi](QVector<sf::Root::SPtr> const& roots)
    {
        auto root = choose(roots);
        if (root)
        {
            connection_helper_.connect_future(
                     get_storage_framework_folder(root, KEEPER_FOLDER, false),
                     std::function<void(sf::Folder::SPtr const &)>{
                          [this, fi, root](sf::Folder::SPtr const & keeper_folder){
                              QVector<QString> res;
                              if (keeper_folder)
                              {
                                  qDebug() << "Keeper root folder was found";
                                  connection_helper_.connect_future(
                                          get_storage_framework_dirs(keeper_folder),
                                          std::function<void(QVector<QString> const &)> {
                                              [this, fi, res](QVector<QString> const & keeper_folders){
                                                  QFutureInterface<decltype(res)> qfi(fi);
                                                  qfi.reportResult(keeper_folders);
                                                  qfi.reportFinished();
                                              }
                                          }
                                  );
                              }
                              else
                              {
                                  qWarning() << "Keeper root folder was not found";
                                  QFutureInterface<decltype(res)> qfi(fi);
                                  qfi.reportResult(res);
                                  qfi.reportFinished();
                              }
                          }
                    }
            );
        }
        else
        {
            qDebug() << "No dirs were found";
            QVector<QString> res;
            QFutureInterface<decltype(res)> qfi(fi);
            qfi.reportResult(res);
            qfi.reportFinished();
        }
    });
    return fi.future();
}

keeper::Error
StorageFrameworkClient::get_last_error() const
{
    return last_error_;
}

QFuture<QStringList>
StorageFrameworkClient::get_accounts()
{
    QFutureInterface<QStringList> fi;
    add_accounts_task([this, fi](QVector<sf::Account::SPtr> const& accounts)
    {
        QFutureInterface<QStringList> qfi(fi);
        QStringList ret_accounts;
        for (auto& account: accounts)
        {
            ret_accounts << get_account_id(account);
        }
        qfi.reportResult(ret_accounts);
        qfi.reportFinished();
    });

    return fi.future();
}

QFuture<sf::Folder::SPtr>
StorageFrameworkClient::get_keeper_folder(sf::Folder::SPtr const & root, QString const & dir_name, bool create_if_not_exists)
{
    QFutureInterface<sf::Folder::SPtr> fi;

    connection_helper_.connect_future(
        get_storage_framework_folder(root, KEEPER_FOLDER, create_if_not_exists),
        std::function<void(sf::Folder::SPtr const &)>{
            [this, fi, root, dir_name, create_if_not_exists](sf::Folder::SPtr const & keeper_folder){
                if (!keeper_folder)
                {
                    qWarning() << "Error creating keeper root folder: " << dir_name;
                    sf::Folder::SPtr ret;
                    QFutureInterface<decltype(ret)> qfi(fi);
                    qfi.reportResult(ret);
                    qfi.reportFinished();
                }
                else
                {
                    connection_helper_.connect_future(
                        get_storage_framework_folder(keeper_folder, dir_name, create_if_not_exists),
                        std::function<void(sf::Folder::SPtr const &)>{
                            [this, fi, root](sf::Folder::SPtr const & timestamp_folder){
                                if (!timestamp_folder)
                                {
                                    qWarning() << "Error creating keeper time stamp folder";
                                }
                                QFutureInterface<sf::Folder::SPtr> qfi(fi);
                                qfi.reportResult(timestamp_folder);
                                qfi.reportFinished();
                            }
                        }
                    );
                }
            }
        }
    );

    return fi.future();
}

QFuture<sf::Folder::SPtr>
StorageFrameworkClient::get_storage_framework_folder(sf::Folder::SPtr const & root,
                                                     QString const & dir_name,
                                                     bool create_if_not_exists)
{
    QFutureInterface<sf::Folder::SPtr> fi;

    connection_helper_.connect_future(
        root->lookup(dir_name),
        std::function<void(QVector<sf::Item::SPtr> const &)>{
            [this, fi, root, dir_name, create_if_not_exists](QVector<sf::Item::SPtr> const & item){
                if (item.size())
                {
                    auto it = item.at(0);
                    // the folder already exists
                    auto ret_root = std::dynamic_pointer_cast<sf::Folder>(item.at(0));
                    QFutureInterface<decltype(ret_root)> qfi(fi);
                    qfi.reportResult(ret_root);
                    qfi.reportFinished();
                }
                else
                {
                    sf::Folder::SPtr res;
                    if (!create_if_not_exists)
                    {
                        QFutureInterface<decltype(res)> qfi(fi);
                        qfi.reportResult(res);
                        qfi.reportFinished();
                        last_error_ = keeper::Error::REMOTE_DIR_NOT_EXISTS;
                    }
                    else
                    {
                        // we need to create the folder
                        connection_helper_.connect_future(
                            root->create_folder(dir_name),
                            std::function<void(sf::Folder::SPtr const &)>{
                                [this, fi, res, root](sf::Folder::SPtr const & folder){
                                    if (!folder)
                                    {
                                        last_error_ = keeper::Error::CREATING_REMOTE_DIR;
                                    }
                                    QFutureInterface<decltype(res)> qfi(fi);
                                    qfi.reportResult(folder);
                                    qfi.reportFinished();
                                }
                            }
                        );
                    }
                }
            }
        }
    );

    return fi.future();
}

QFuture<sf::File::SPtr>
StorageFrameworkClient::get_storage_framework_file(sf::Folder::SPtr const & root, QString const & file_name)
{
    QFutureInterface<sf::File::SPtr> fi;

    connection_helper_.connect_future(
        root->lookup(file_name),
        std::function<void(QVector<sf::Item::SPtr> const &)>{
            [this, fi, root, file_name](QVector<sf::Item::SPtr> const & item){
                if (item.size())
                {
                    qDebug() << "Storage framework file " << file_name << " under folder: " << root->name() << " exists";
                    auto it = item.at(0);
                    // the item exists...
                    // do a dynamic_cast and return the result
                    auto ret_file = std::dynamic_pointer_cast<sf::File>(item.at(0));
                    QFutureInterface<decltype(ret_file)> qfi(fi);
                    qfi.reportResult(ret_file);
                    qfi.reportFinished();
                }
                else
                {
                    qDebug() << "Storage framework file " << file_name << " under folder: " << root->name() << "does not exist";
                    sf::File::SPtr res;
                    QFutureInterface<decltype(res)> qfi(fi);
                    qfi.reportResult(res);
                    qfi.reportFinished();
                }
            }
        }
    );

    return fi.future();
}

QFuture<QVector<QString>>
StorageFrameworkClient::get_storage_framework_dirs(unity::storage::qt::client::Folder::SPtr const & root)
{
    QFutureInterface<QVector<QString>> fi;

    connection_helper_.connect_future(
        root->list(),
        std::function<void(QVector<sf::Item::SPtr> const &)>{
            [this, fi, root](QVector<sf::Item::SPtr> const & items){
                QVector<QString> res;

                for (auto item : items)
                {
                    if (item->type() == unity::storage::ItemType::folder)
                    {
                        res.push_back(item->name());
                    }
                }

                QFutureInterface<decltype(res)> qfi(fi);
                qfi.reportResult(res);
                qfi.reportFinished();
            }
        }
    );

    return fi.future();
}

void
StorageFrameworkClient::clear_last_error()
{
    last_error_ = keeper::Error::OK;
}

QString
StorageFrameworkClient::get_account_id(unity::storage::qt::client::Account::SPtr const & account)
{
    return QStringLiteral("%1:%2").arg(account->owner_id()).arg(account->description());
}
