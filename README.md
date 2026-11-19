# Cinemania

A MediaTracker video export mod for TrackMania United Forever.

## Features

- Up to 10× faster video rendering
- MP4 export with optional sound
- HD replays for smoother car motion

## Requirements

- 64-bit Windows
- TrackMania United Forever 2.12.0
- [TMLoader 1.0.1 or newer](https://tomashu.dev/software/tmloader/)

## Install

1. Close TMLoader.
   
2. Open `%LOCALAPPDATA%\TMLoader\config.yaml` and add the URL under `servers:`, keeping any existing entries:

   ```yaml
   servers:
     - 'https://raw.githubusercontent.com/mediocre-q/Cinemania/master/modloader/'
   ```

3. Reopen TMLoader and select Cinemania in your profile.
   
4. Export a video normally through MediaTracker.

## Record an HD replay

**Record HD Replay** captures the car's movement every 10 ms. The denser samples make the car move more smoothly in rendered footage, especially at high frame rates or in slow motion. The recording includes the full three-second braking outro.

1. Select a replay in the replay editor.
2. Select **Record HD Replay**. Cinemania replays the inputs at a high sample rate and saves `<original_name>_HD.Replay.Gbx` beside the source. If that HD copy already exists, Cinemania replaces it.
3. Select the new `_HD` replay and choose **Shoot video** to render it or edit it in the MediaTracker and render from there.
