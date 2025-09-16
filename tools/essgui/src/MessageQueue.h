class MessageQueue {
    std::queue<std::string> messages;
    mutable std::mutex mutex;
    
public:
    std::atomic<bool> awake_pending{false};
    void push(std::string msg) {
      void drain_queue_cb(void*);
        {
            std::lock_guard<std::mutex> lock(mutex);
            messages.push(std::move(msg));
        }
        
        bool expected = false;
        if (awake_pending.compare_exchange_strong(expected, true)) {
            Fl::awake(drain_queue_cb, nullptr);
        }
    }
    
    bool pop(std::string& msg) {
        std::lock_guard<std::mutex> lock(mutex);
        if (messages.empty()) return false;
        msg = std::move(messages.front());
        messages.pop();
        return true;
    }
    
    bool empty() const {  // <-- Add this method
        std::lock_guard<std::mutex> lock(mutex);
        return messages.empty();
    }
    
};

MessageQueue g_message_queue;
void process_single_message(const std::string& dpoint);

void drain_queue_cb(void*) {
    std::string msg;
    int processed = 0;
    
    while (processed < 10 && g_message_queue.pop(msg)) {
        process_single_message(msg);
        processed++;
    }
    
    g_message_queue.awake_pending = false;
    if (!g_message_queue.empty()) {  // Now this will compile
        bool expected = false;
        if (g_message_queue.awake_pending.compare_exchange_strong(expected, true)) {
            Fl::awake(drain_queue_cb, nullptr);
        }
    }
}
