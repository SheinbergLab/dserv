#include <unordered_map>
#include <mutex>
#include <memory>
#include <string>

// Thread safe registry with regular mutex
template<typename T>
class ObjectRegistry {
private:
    std::unordered_map<std::string, T*> objects;
    mutable std::mutex mutex_;
    
public:
    void registerObject(const std::string& name, T* obj) {
        std::lock_guard<std::mutex> lock(mutex_);
        objects[name] = obj;
    }
    
    void unregisterObject(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        objects.erase(name);
    }
    
    T* getObject(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = objects.find(name);
        return (it != objects.end()) ? it->second : nullptr;
    }
    
    std::unordered_map<std::string, T*> getAllObjects() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return objects;
    }
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return objects.size();
    }
    
    bool exists(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return objects.find(name) != objects.end();
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        objects.clear();
    }
};
