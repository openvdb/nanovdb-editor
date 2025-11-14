// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/editor/EditorToken.h

    \author Andrew Reidmeyer, Petra Hapalova

    \brief  Token management system for efficient string lookups in the editor.
*/

#ifndef NANOVDB_EDITOR_TOKEN_H_HAS_BEEN_INCLUDED
#define NANOVDB_EDITOR_TOKEN_H_HAS_BEEN_INCLUDED

#include "nanovdb_editor/putil/Editor.h"

#include <unordered_map>
#include <mutex>
#include <string>
#include <memory>
#include <deque>

namespace pnanovdb_editor
{
/*!
    \brief Thread-safe token manager for efficient string lookups.

    The EditorToken class provides a singleton token management system that:
    - Converts strings to unique tokens with persistent IDs
    - Caches tokens to avoid repeated allocations
    - Provides O(1) average lookup time
    - Ensures same string always returns the same token
    - Thread-safe for concurrent access

    Tokens consist of:
    - A unique 64-bit ID (starting from 1)
    - A pointer to the original string (owned by the manager)

    Implementation details:
    - Strings are stored in a std::deque for stable pointer guarantees
    - Unlike std::vector, std::deque never invalidates pointers to existing elements
    - This ensures token->str remains valid even as new tokens are added

    Usage:
    \code
    auto* token = EditorToken::getInstance().getToken("my_scene");
    // Same string returns the same token
    auto* token2 = EditorToken::getInstance().getToken("my_scene");
    assert(token == token2);
    \endcode
*/
class EditorToken
{
public:
    /*!
        \brief Get the singleton instance.
        \return Reference to the singleton EditorToken instance.
    */
    static EditorToken& getInstance()
    {
        static EditorToken instance;
        return instance;
    }

    /*!
        \brief Get or create a token for the given string.

        If a token already exists for this string, returns the existing token.
        Otherwise, creates a new token with a unique ID.

        \param name The string to convert to a token. If NULL, returns NULL.
        \return Pointer to the token, or NULL if name is NULL.

        \note Thread-safe. The returned pointer remains valid for the lifetime of the EditorToken instance.
    */
    pnanovdb_editor_token_t* getToken(const char* name)
    {
        if (!name)
        {
            return nullptr;
        }

        std::string key(name);

        // First, try to find existing token
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_tokens.find(key);
            if (it != m_tokens.end())
            {
                return it->second.get();
            }
        }

        // Token doesn't exist, create a new one
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // Double-check in case another thread created it while we were waiting
            auto it = m_tokens.find(key);
            if (it != m_tokens.end())
            {
                return it->second.get();
            }

            // Create new token
            auto token = std::make_unique<pnanovdb_editor_token_t>();
            token->id = m_nextId++;

            // Store the string persistently
            m_strings.push_back(key);
            token->str = m_strings.back().c_str();

            auto* token_ptr = token.get();
            m_tokens[key] = std::move(token);

            return token_ptr;
        }
    }

    /*!
        \brief Get a token by its unique ID.

        \param id The unique ID to search for.
        \return Pointer to the token with the given ID, or NULL if not found.

        \note Thread-safe. O(n) complexity where n is the number of tokens.
    */
    pnanovdb_editor_token_t* getTokenById(pnanovdb_uint64_t id)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& pair : m_tokens)
        {
            if (pair.second->id == id)
            {
                return pair.second.get();
            }
        }
        return nullptr;
    }

    /*!
        \brief Clear all tokens.

        Removes all tokens and resets the ID counter. Useful for testing or cleanup.

        \note Thread-safe. Invalidates all previously returned token pointers.
    */
    void clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tokens.clear();
        m_strings.clear();
        m_nextId = 1;
    }

    /*!
        \brief Get the total number of tokens.

        \return The number of unique tokens currently stored.

        \note Thread-safe.
    */
    size_t getTokenCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_tokens.size();
    }

private:
    EditorToken() : m_nextId(1)
    {
    }
    ~EditorToken() = default;

    // Delete copy and move constructors/operators
    EditorToken(const EditorToken&) = delete;
    EditorToken& operator=(const EditorToken&) = delete;
    EditorToken(EditorToken&&) = delete;
    EditorToken& operator=(EditorToken&&) = delete;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::unique_ptr<pnanovdb_editor_token_t>> m_tokens;
    std::deque<std::string> m_strings; // Persistent storage for string data (deque maintains stable pointers)
    pnanovdb_uint64_t m_nextId;
};

// Helper function to check if two tokens are equal
static inline bool tokens_equal(pnanovdb_editor_token_t* a, pnanovdb_editor_token_t* b)
{
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    return a->id == b->id;
}

// Helper function to get string from token (safe)
static inline const char* token_to_string(pnanovdb_editor_token_t* token)
{
    return token ? token->str : "";
}

// Helper function to get string from token (safe)
static inline const char* token_to_string_log(pnanovdb_editor_token_t* token)
{
    return token ? token->str : "<null>";
}


// Helper function to check if token is empty
static inline bool token_is_empty(pnanovdb_editor_token_t* token)
{
    return !token || !(token->str) || token->str[0] == '\0';
}

} // namespace pnanovdb_editor

#endif // NANOVDB_EDITOR_TOKEN_H_HAS_BEEN_INCLUDED
