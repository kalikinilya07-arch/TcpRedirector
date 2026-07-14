#include "ContextStore.h"
#include <cstdio>

namespace tcp_redirector {
namespace auth_agent {

ContextStore::~ContextStore() {
    Clear();
}

uint64_t ContextStore::Create(CredHandle credentials, CtxtHandle context,
                               std::string spn, std::chrono::seconds ttl) {
    std::lock_guard<std::mutex> lock(m_mutex);

    uint64_t id = m_nextId++;

    ContextEntry entry;
    entry.credentials = credentials;
    entry.context = context;
    entry.spn = std::move(spn);
    entry.created_at = std::chrono::steady_clock::now();
    entry.ttl = ttl;
    entry.in_use = true;

    m_contexts[id] = std::move(entry);
    return id;
}

ContextEntry* ContextStore::Get(uint64_t id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_contexts.find(id);
    if (it == m_contexts.end()) {
        return nullptr;
    }

    if (it->second.IsExpired()) {
        DestroyContext(it->second);
        m_contexts.erase(it);
        return nullptr;
    }

    return &it->second;
}

void ContextStore::Close(uint64_t id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_contexts.find(id);
    if (it == m_contexts.end()) {
        return;
    }

    DestroyContext(it->second);
    m_contexts.erase(it);
}

void ContextStore::CleanupExpired() {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto it = m_contexts.begin(); it != m_contexts.end(); ) {
        if (it->second.IsExpired()) {
            DestroyContext(it->second);
            it = m_contexts.erase(it);
        } else {
            ++it;
        }
    }
}

void ContextStore::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& [id, entry] : m_contexts) {
        DestroyContext(entry);
    }
    m_contexts.clear();
}

size_t ContextStore::Size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_contexts.size();
}

void ContextStore::DestroyContext(ContextEntry& entry) {
    if (entry.in_use) {
        DeleteSecurityContext(&entry.context);
        FreeCredentialsHandle(&entry.credentials);
        entry.in_use = false;
    }
}

} // namespace auth_agent
} // namespace tcp_redirector