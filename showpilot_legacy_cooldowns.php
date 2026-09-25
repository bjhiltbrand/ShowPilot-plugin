<?php
// ============================================================
// ShowPilot — restore songs hidden by pre-0.14 cooldowns
// ============================================================
// Before v0.14.0, the "skip cooled-down songs in FPP's rotation" option
// removed songs from the operator's playlist file and put them back when the
// cooldown ended, tracking them in a state file. v0.14.0 skips them with
// FPP's "Next Playlist Item" command instead and never edits playlists.
//
// This file only undoes the old behaviour: restoreAllCooldowns() puts every
// still-hidden song back at its original position and deletes the state
// file. The listener calls it once at startup and fpp_uninstall.sh calls it
// via scripts/restore_cooldowns.php. It is a no-op when there is no state.
// Requires showpilot_common.php.
// ============================================================

// State file, pre-0.14: config/showpilot-cooldowns.json, then briefly
// plugindata/<repoName>/cooldowns.json. Layout:
//   { "snapshot":  { "<playlist>": [ ...mainPlaylist at show start... ] },
//     "cooldowns": { "<sequence>": { "reenableAt": "...", "playlist": "<playlist>" } } }
function legacyCooldownFiles() {
    global $settings;
    return array(
        $settings['configDirectory'] . '/showpilot-cooldowns.json',
        sp_data_dir() . '/cooldowns.json',
    );
}

function loadLegacyCooldownState() {
    $state = array('snapshot' => array(), 'cooldowns' => array());
    foreach (legacyCooldownFiles() as $file) {
        $data = json_decode((string)@file_get_contents($file), true);
        if (!is_array($data)) continue;
        foreach (array('snapshot', 'cooldowns') as $key) {
            if (isset($data[$key]) && is_array($data[$key])) $state[$key] += $data[$key];
        }
    }
    return $state;
}

function reinsertFromSnapshot($name, $playlist, &$data, &$state) {
    // Find original entry and index in snapshot
    if (!isset($state['snapshot'][$playlist])) {
        sp_log("[cooldown] WARN - An older ShowPilot version removed '$name' from playlist '$playlist' and its original entry was not kept, so it cannot be restored automatically. Re-add it in FPP's playlist editor.");
        unset($state['cooldowns'][$name]);
        return false;
    }

    $snapshot = $state['snapshot'][$playlist];
    $origIndex = null;
    $origEntry = null;
    foreach ($snapshot as $idx => $item) {
        $entryFile = isset($item['sequenceName']) ? $item['sequenceName']
                   : (isset($item['mediaName']) ? $item['mediaName'] : '');
        if (pathinfo($entryFile, PATHINFO_FILENAME) === $name) {
            $origIndex = $idx;
            $origEntry = $item;
            break;
        }
    }

    if ($origEntry === null) {
        sp_log("[cooldown] WARN - An older ShowPilot version removed '$name' from playlist '$playlist' and its original entry was not kept, so it cannot be restored automatically. Re-add it in FPP's playlist editor.");
        unset($state['cooldowns'][$name]);
        return false;
    }

    // Don't re-insert if it's already in the live playlist (avoid duplicates)
    foreach ($data['mainPlaylist'] as $item) {
        $entryFile = isset($item['sequenceName']) ? $item['sequenceName']
                   : (isset($item['mediaName']) ? $item['mediaName'] : '');
        if (pathinfo($entryFile, PATHINFO_FILENAME) === $name) {
            // Already present — just clear the cooldown state
            unset($state['cooldowns'][$name]);
            return false;
        }
    }

    // Insert at original index, clamped to current array length
    $insertAt = min($origIndex, count($data['mainPlaylist']));
    array_splice($data['mainPlaylist'], $insertAt, 0, array($origEntry));
    unset($state['cooldowns'][$name]);
    sp_log("[cooldown] Re-inserted '$name' into playlist '$playlist' at index $insertAt");
    return true;
}

function restoreAllCooldowns() {
    $state = loadLegacyCooldownState();
    $byPlaylist = array();
    foreach ($state['cooldowns'] as $name => $entry) {
        if (is_array($entry) && isset($entry['playlist']) && is_string($entry['playlist'])) {
            $byPlaylist[$entry['playlist']][] = (string)$name;
        }
    }
    foreach ($byPlaylist as $playlist => $names) {
        $data = sp_read_playlist($playlist);
        if ($data === null || !isset($data['mainPlaylist'])) continue;
        $changed = false;
        foreach ($names as $name) {
            if (reinsertFromSnapshot($name, $playlist, $data, $state)) $changed = true;
        }
        if ($changed && !sp_write_playlist($playlist, $data)) {
            sp_log("[cooldown] ERROR: could not restore playlist '$playlist'");
            continue;
        }
    }
    foreach (legacyCooldownFiles() as $file) @unlink($file);
}
