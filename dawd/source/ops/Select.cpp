#include "Ops.h"
#include "../EngineHelpers.h"

// Select: Favoriten auf Material (Clips und Sections) — kind select am
// Node, abgelegt als select-Aktionen in tree.json. Adressierung wie die
// Clip-Edit-Ops: track + name aus state.trackList, Sections ueber den
// Namen. on fehlt -> togglen; der Zustand je Ziel entscheidet die letzte
// select-Aktion (TreeState::lastSelectOn) und leitet sich in state und
// treeState ab — der Tree-Filter "nur Favoriten" zeigt auch diese Nodes.
namespace dawd
{
    juce::var opFavoriteClip (DawdContext& ctx, const juce::var& cmd)
    {
        auto trackIndex = (int) (double) cmd.getProperty ("track", -1.0);
        auto name = cmd.getProperty ("name", {}).toString();

        if (name.isEmpty())
            return resultObj (false, { { "error", "no clip name" } });

        auto tracks = userTracks (*ctx.edit);

        if (trackIndex < 0 || trackIndex >= tracks.size())
            return resultObj (false, { { "error", "no such track" } });

        if (findClipByName (*tracks[trackIndex], name) == nullptr)
            return resultObj (false, { { "error", "no such clip: " + name } });

        auto& t = *ctx.tree;
        auto* node = t.find (t.current);
        auto on = cmd.getProperty ("on", {});
        auto favoriting = on.isVoid()
                              ? ! TreeNode::lastSelectOn (node->actions, "favoriteClip", name, trackIndex)
                              : (bool) on;

        auto params = new juce::DynamicObject();
        params->setProperty ("track", trackIndex);
        params->setProperty ("name", name);
        params->setProperty ("on", favoriting);
        recordNodeAction (ctx, "select", "favoriteClip", juce::var (params));

        return resultObj (true, { { "track", trackIndex }, { "name", name },
                                  { "favorite", favoriting } });
    }

    juce::var opFavoriteSection (DawdContext& ctx, const juce::var& cmd)
    {
        auto name = cmd.getProperty ("name", {}).toString();

        if (name.isEmpty())
            return resultObj (false, { { "error", "no section name" } });

        if (findSection (*ctx.edit, name) == nullptr)
            return resultObj (false, { { "error", "no such section: " + name } });

        auto& t = *ctx.tree;
        auto* node = t.find (t.current);
        auto on = cmd.getProperty ("on", {});
        auto favoriting = on.isVoid()
                              ? ! TreeNode::lastSelectOn (node->actions, "favoriteSection", name, -1)
                              : (bool) on;

        auto params = new juce::DynamicObject();
        params->setProperty ("name", name);
        params->setProperty ("on", favoriting);
        recordNodeAction (ctx, "select", "favoriteSection", juce::var (params));

        return resultObj (true, { { "name", name }, { "favorite", favoriting } });
    }
}
