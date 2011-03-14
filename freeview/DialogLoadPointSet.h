/**
 * @file  DialogLoadPointSet.h
 * @brief REPLACE_WITH_ONE_LINE_SHORT_DESCRIPTION
 *
 */
/*
 * Original Author: Ruopeng Wang
 * CVS Revision Info:
 *    $Author: nicks $
 *    $Date: 2011/03/14 23:44:46 $
 *    $Revision: 1.7 $
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
#ifndef DIALOGLOADPOINTSET_H
#define DIALOGLOADPOINTSET_H

#include <QDialog>

namespace Ui
{
class DialogLoadPointSet;
}

class DialogLoadPointSet : public QDialog
{
  Q_OBJECT
public:
  explicit DialogLoadPointSet(QWidget *parent = 0);
  ~DialogLoadPointSet();

  QStringList GetFileNames();

  int GetPointSetType();

  void SetLastDir( const QString& dir )
  {
    m_strLastDir = dir;
  }

protected slots:
  void OnOK();
  void OnButtonOpen();

private:
  Ui::DialogLoadPointSet *ui;
  QString m_strLastDir;
};

#endif // DIALOGLOADPOINTSET_H
