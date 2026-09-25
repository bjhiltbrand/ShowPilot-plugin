<?php
// Run by fpp_uninstall.sh: put back any song a pre-0.14 listener hid from a
// playlist for a ShowPilot cooldown, so uninstalling never leaves the
// operator's playlist missing songs.
if (php_sapi_name() !== 'cli') exit;
$skipJSsettings = true;
require_once dirname(__DIR__) . '/showpilot_common.php';
require_once dirname(__DIR__) . '/showpilot_legacy_cooldowns.php';

restoreAllCooldowns();
