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
 *   Xavi Garcia <xavi.garcia.mena@canoincal.com>
 *   Charles Kerr <charles.kerr@canoincal.com>
 */
#pragma once

#include "keeper-task.h"

class KeeperTaskBackupPrivate;

class KeeperTaskBackup : public KeeperTask
{
    Q_OBJECT
    Q_DECLARE_PRIVATE(KeeperTaskBackup)
public:

    KeeperTaskBackup(TaskData const & task_data,
               QSharedPointer<HelperRegistry> const & helper_registry,
               QSharedPointer<StorageFrameworkClient> const & storage,
               QObject *parent = nullptr);
    virtual ~KeeperTaskBackup();

    Q_DISABLE_COPY(KeeperTaskBackup)

    void ask_for_storage_framework_socket(quint64 n_bytes);

protected:
    QStringList get_helper_urls() const override;
    void init_helper() override;

};
