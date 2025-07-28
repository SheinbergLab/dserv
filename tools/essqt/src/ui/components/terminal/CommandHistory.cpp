#include "CommandHistory.h"

CommandHistory::CommandHistory(int maxSize)
    : m_maxSize(maxSize)
    , m_currentIndex(-1)
{
}

void CommandHistory::add(const QString &command)
{
    if (command.isEmpty()) return;
    
    // Don't add duplicates of the last command
    if (!m_history.isEmpty() && m_history.last() == command) {
        return;
    }
    
    m_history.append(command);
    
    // Maintain size limit
    while (m_history.size() > m_maxSize) {
        m_history.removeFirst();
    }
    
    resetNavigation();
}

QString CommandHistory::getPrevious()
{
    if (m_history.isEmpty()) return QString();
    
    if (m_currentIndex == -1) {
        m_currentIndex = m_history.size() - 1;
    } else if (m_currentIndex > 0) {
        m_currentIndex--;
    }
    
    return m_history.at(m_currentIndex);
}

QString CommandHistory::getNext()
{
    if (m_currentIndex == -1) return QString();
    
    m_currentIndex++;
    
    if (m_currentIndex >= m_history.size()) {
        m_currentIndex = -1;
        return m_tempCommand;
    }
    
    return m_history.at(m_currentIndex);
}

void CommandHistory::resetNavigation()
{
    m_currentIndex = -1;
    m_tempCommand.clear();
}

void CommandHistory::clear()
{
    m_history.clear();
    resetNavigation();
}
