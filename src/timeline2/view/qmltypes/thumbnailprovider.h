/*
    SPDX-FileCopyrightText: 2013-2016 Meltytech LLC
    SPDX-FileCopyrightText: Dan Dennedy <dan@dennedy.org>

    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <KImageCache>
#include <QCache>
#include <QMutex>
#include <QQuickImageProvider>
#include <QReadWriteLock>
#include <memory>
#include <mlt++/MltProducer.h>
#include <mlt++/MltProfile.h>

class ThumbnailProvider : public QQuickImageProvider
{
public:
    explicit ThumbnailProvider();
    ~ThumbnailProvider() override;
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
    /** @brief Refuse further requests and wait until running ones are finished, must be called before closing MLT */
    static void shutdown();

private:
    Mlt::Profile m_profile;
    /** @brief Thumbnails of single multicam angles, keyed by request id (which changes when the sequence is modified) */
    QCache<QString, QImage> m_angleCache{64 * 1024 * 1024};
    QMutex m_angleCacheMutex;
    /** @brief Requests run in QML's pixmap reader threads, which outlive the timeline */
    static QReadWriteLock s_shutdownLock;
    static bool s_shuttingDown;
    QImage makeThumbnail(std::unique_ptr<Mlt::Producer> producer, int frameNumber, const QSize &requestedSize);
};
