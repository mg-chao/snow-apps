# Administrator launch: interactive Windows verification

Run these cases in a disposable Windows VM with a packaged Snow Shot installation,
an administrator account using a filtered UAC token, and a standard account. These
checks intentionally require a person to respond to Windows authorization dialogs.
The automated tests do not approve UAC or modify live startup registrations.

## Settings and startup

1. With ordinary auto-start enabled, verify that **Launch as administrator** is off
   and immediately follows **Auto start at boot**. The next row is **Restart as
   administrator**, showing **Restart**.
2. Enable administrator launch and decline UAC. Expect one error message, the switch
   off, the original Run entry unchanged, and no task for this installation.
3. Enable it and approve UAC. Expect one task named `SnowShot-<user SID>-<path hash>`,
   no `SnowShot` Run entry, and both settings retained after closing and reopening.
   Inspect the task's user SID, interactive logon trigger, highest run level,
   executable, `--autostart` argument, working directory, battery settings, and
   unlimited execution time.
4. Sign out and back in. Expect one elevated Snow Shot process in the user's desktop
   session and no foreground main window caused by auto-start.
5. From an unelevated app, turn administrator launch off and decline UAC. Expect one
   error, the switch still on, and the task unchanged. Approve a second attempt;
   expect the task removed and ordinary registry startup restored.
6. Repeat step 5 by turning **Auto start at boot** off. On cancellation both switches
   retain their previous values. On success both are off, neither registration
   exists, and administrator launch is unavailable.
7. Reset General while administrator launch is enabled. Cancellation must preserve
   the startup settings. Success must restore ordinary auto-start and disable
   administrator launch.
8. Under the standard account, both administrator controls must be unavailable with
   an Administrators-group explanation. With auto-start off under the administrator
   account, the launch switch must instead explain its auto-start dependency.
9. Switch between English, Simplified Chinese, and Traditional Chinese. Verify the
   labels, hints, and operation messages, including the elevated button label.

## Restart and recovery

1. Decline UAC for **Restart**. Expect a warning and the original PID still running.
2. Approve UAC. Expect the original process to exit cleanly, followed by exactly one
   elevated process with the main window open and the same user settings/storage.
3. Verify **Elevated** uses the semantic success color, stays enabled, and does not
   change the PID or show UAC when clicked, in both light and dark themes.
4. While recording/exporting or handing off an update, verify that restart cannot
   close the active work. Verify duplicate requests do not create extra helpers.
5. In the VM, interrupt a startup transition. On the next launch, verify that an
   inconsistent registration produces an actionable recovery message without a
   silent UAC prompt or a second startup mode. Reapplying the setting with consent
   must clear the recovery record and establish the selected mode.

## Installer and updater

1. Update from both an ordinary and an elevated app. The restarted app must retain
   the original privilege level. Declining required update authorization must leave
   the original app and installation running unchanged.
2. Update a machine-wide installation from the standard account, supplying a
   different administrator's credentials. Verify that only the worker changes
   account and the restarted app uses the original account and its settings.
3. Upgrade an installation with elevated auto-start, both in place and to a new
   directory. Verify that the startup choice survives and its executable target
   points to the final installation. No obsolete task should remain at the old path.
4. Use the installer's finish-page launch. Verify the app starts on the interactive
   desktop without inheriting the installer token. If the desktop shell is
   unavailable, verify the installer reports the launch failure and leaves the app
   closed.
5. Uninstall with ordinary/elevated startup registrations belonging to signed-in and
   signed-out users. Only registrations targeting this installation should be
   removed. Verify permission or running-process cleanup failure stops destructive
   file removal and reports an error.

Record Windows version, account type, installation variant, initial/final PIDs and
privilege levels, task/registry state, and observed messages for each case.
