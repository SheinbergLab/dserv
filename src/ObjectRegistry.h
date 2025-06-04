
template<typename T>
class ObjectRegistry {
private:
    std::unordered_map<std::string, T*> objects;
    
public:
    void registerObject(const std::string& name, T* obj) {
        objects[name] = obj;
    }
    
    void unregisterObject(const std::string& name) {
        objects.erase(name);
    }
    
    T* getObject(const std::string& name) {
        auto it = objects.find(name);
        return (it != objects.end()) ? it->second : nullptr;
    }
    
    const std::unordered_map<std::string, T*>& getAllObjects() const {
        return objects;
    }
    
    size_t size() const { return objects.size(); }
};
