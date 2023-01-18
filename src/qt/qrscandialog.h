// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_QRSCANDIALOG_H
#define BITCOIN_QT_QRSCANDIALOG_H

#include <QDialog>

namespace Ui {
    class QRScanDialog;
}

/* Dialog showing transaction details. */
class QRScanDialog : public QDialog
{
    Q_OBJECT

public:
    explicit QRScanDialog(QWidget *parent = nullptr);
    virtual ~QRScanDialog();

private Q_SLOTS:
    void on_scanFileButton_clicked();
    void on_scanCameraButton_clicked();

private:
    Ui::QRScanDialog *ui;
};

#endif // BITCOIN_QT_QRSCANDIALOG_H
