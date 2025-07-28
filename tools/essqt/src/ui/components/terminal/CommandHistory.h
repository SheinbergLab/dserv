#pragma once

#include <QString>
#include <QStringList>

class CommandHistory
{
public:
    CommandHistory(int maxSize = 1000);
    
    void add(const QString &command);
    QString getPrevious();
    QString getNext();
    void resetNavigation();
    
    QStringList history() const { return m_history; }
    void clear();

    int currentIndex() const { return m_currentIndex; }
    void setTempCommand(const QString &cmd) { m_tempCommand = cmd; }
  
private:
    QStringList m_history;
    int m_maxSize;
    int m_currentIndex;
    QString m_tempCommand;
};
