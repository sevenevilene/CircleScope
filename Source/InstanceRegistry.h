#pragma once

#include <juce_core/juce_core.h>
#include <memory>
#include <vector>
#include "ScopeSource.h"

// Every CircleScope instance in this process. A host loads the plugin once
// and runs all its instances in one process, so through this list an
// instance can see the others: show them by name, and add their audio to
// its own drawing. (A host that gives every plugin a process of its own
// leaves each instance alone in its list.)
//
// An instance is known by an id it keeps in its saved state, so a choice of
// other instances survives closing and reopening the session. The sources
// are shared: whoever is still reading one when its instance goes keeps it
// alive until done.
class InstanceRegistry
{
public:
    struct Entry
    {
        juce::Uuid id;
        juce::String name;                       // the track's, if the host says
        int number = 0;                          // in order of creation, for a name when there is none
        std::shared_ptr<ScopeSource> source;

        juce::String displayName() const { return name.isNotEmpty() ? name : "Instance " + juce::String (number); }
    };

    static InstanceRegistry& get()
    {
        static InstanceRegistry registry;
        return registry;
    }

    void add (const juce::Uuid& id, std::shared_ptr<ScopeSource> source)
    {
        const juce::ScopedLock lock (mutex);
        entries.push_back ({ id, {}, ++lastNumber, std::move (source) });
    }

    void remove (const ScopeSource* source)
    {
        const juce::ScopedLock lock (mutex);
        entries.erase (std::remove_if (entries.begin(), entries.end(), [source] (const Entry& e) { return e.source.get() == source; }),
                       entries.end());
    }

    // Gives an instance the id from its saved state. False if another
    // instance has it already (a duplicated track): then it keeps its own.
    bool changeId (const ScopeSource* source, const juce::Uuid& id)
    {
        const juce::ScopedLock lock (mutex);
        for (const auto& e : entries)
            if (e.id == id && e.source.get() != source)
                return false;
        for (auto& e : entries)
            if (e.source.get() == source)
                e.id = id;
        return true;
    }

    void setName (const ScopeSource* source, const juce::String& name)
    {
        const juce::ScopedLock lock (mutex);
        for (auto& e : entries)
            if (e.source.get() == source)
                e.name = name;
    }

    std::vector<Entry> list() const
    {
        const juce::ScopedLock lock (mutex);
        return entries;
    }

    std::shared_ptr<ScopeSource> find (const juce::Uuid& id) const
    {
        const juce::ScopedLock lock (mutex);
        for (const auto& e : entries)
            if (e.id == id)
                return e.source;
        return nullptr;
    }

private:
    juce::CriticalSection mutex;
    std::vector<Entry> entries;
    int lastNumber = 0;
};
