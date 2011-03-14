/**
 * @file  DialogSaveVolume.cpp
 * @brief REPLACE_WITH_ONE_LINE_SHORT_DESCRIPTION
 *
 */
/*
 * Original Author: Ruopeng Wang
 * CVS Revision Info:
 *    $Author: nicks $
 *    $Date: 2011/03/14 23:44:47 $
 *    $Revision: 1.4 $
 *
 * Copyright © 2011 The General Hospital Corporation (Boston, MA) "MGH"
 *
 * Terms and conditions for use, reproduction, distribution and contribution
 * are found in the 'FreeSurfer Software License Agreement' contained
 * in the file 'LICENSE' found in the FreeSurfer distribution, and here:
 *
 * https://surfer.nmr.mgh.harvard.edu/fswiki/FreeSurferSoftwareLicense
 *
 * Reporting: freesurfer@nmr.mgh.harvard.edu
 *
 */
#include "DialogSaveVolume.h"
#include "ui_DialogSaveVolume.h"
#include <QMessageBox>
#include <QFileDialog>
#include <QFile>

DialogSaveVolume::DialogSaveVolume(QWidget *parent, const QString& filepath) :
  QDialog(parent),
  ui(new Ui::DialogSaveVolume)
{
  ui->setupUi(this);
  ui->lineEditFileName->setText(filepath);
}

DialogSaveVolume::~DialogSaveVolume()
{
  delete ui;
}

QString DialogSaveVolume::GetFileName()
{
  return ui->lineEditFileName->text().trimmed();
}

bool DialogSaveVolume::GetResample()
{
  return !ui->checkBoxNoResample->isChecked();
}

void DialogSaveVolume::OnOK()
{
  if (GetFileName().isEmpty())
  {
    QMessageBox::warning(this, "Error", "Please enter the file name to save.");
  }
  else
  {
    if (QFile::exists(GetFileName()))
    {
      if (QMessageBox::question(this, "File exists", "File exists. Do you want to overwrite it?", QMessageBox::Yes, QMessageBox::No) ==
          QMessageBox::Yes)
      {
        accept();
      }
    }
    else
    {
      accept();
    }
  }
}

void DialogSaveVolume::OnOpen()
{
  QString fn = QFileDialog::getSaveFileName( this, "Save volume",
               GetFileName(),
               "Volume files (*.mgz *.mgh *.nii *.nii.gz *.img *.mnc);;All files (*.*)");
  if ( !fn.isEmpty() )
  {
    ui->lineEditFileName->setText(fn);
    ui->lineEditFileName->setCursorPosition(fn.length());
  }
}
