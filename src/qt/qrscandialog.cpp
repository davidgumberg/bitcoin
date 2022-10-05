// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <QFileDialog>
#include <QCamera>

#include <qt/qrscandialog.h>
#include <qt/forms/ui_qrscandialog.h>

#include <qt/guiutil.h>

#include <qt/QZXing.h>

#include <iostream>

QRScanDialog::QRScanDialog(QWidget *parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::QRScanDialog)
{
    ui->setupUi(this);
    setWindowTitle("Scan QR Code");

    GUIUtil::handleCloseWindowShortcut(this);
}

QRScanDialog::~QRScanDialog()
{
    delete ui;
}

void QRScanDialog::on_scanFileButton_clicked()
{
	QString fileName = QFileDialog::getOpenFileName(this, "Select an image file");
	if(fileName.isNull() || fileName.isEmpty()){
		std::cout << "somebody nullt the whole darn guy \n";
		return;
	}

	std::cout << fileName.toUtf8().data();
	std::cout << "Da dog runt whilt" << std::endl;
	QImage imageToDecode(fileName);
	if(imageToDecode.isNull()){
		std::cout << "Somebody nullt it: " << fileName.toStdString() << "now with data" << fileName.toStdString().data() << std::endl;
		return;
	}
	QZXing decoder;

	decoder.setDecoder( QZXing::DecoderFormat_QR_CODE | QZXing::DecoderFormat_EAN_13 );
    decoder.setTryHarderBehaviour(QZXing::TryHarderBehaviour_ThoroughScanning | QZXing::TryHarderBehaviour_Rotate);

	QString result = decoder.decodeImage(imageToDecode, 360, 360);
	if(result.isNull()){
		std::cout << "death" << std::endl;
		return;
	}
	if(!result.isNull() && !result.isEmpty()){
		std::cout << "hello" << fileName.toUtf8().data() << " " << result.toUtf8().data() << "bubkis" << std::endl;
	}
	else
		std::cout << "Wharr happinet";
}

void QRScanDialog::on_scanCameraButton_clicked()
{
	QString fileName = QFileDialog::getOpenFileName(this, "Select an image file");
}
