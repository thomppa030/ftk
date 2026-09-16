# Skeletons & Clips — `.fjskel` / `.fjanim`

> Stub — fill in.

## Scope
The runtime data behind skeletal animation. Skeletons describe joint hierarchies; clips describe per-joint keyframes. Both are produced by `fjimport` from `.glb`/`.gltf`/`.fbx`.

## Planned sections
- `.fjskel` — joint hierarchy, bind pose, naming
- `.fjanim` — clip data (channels, keyframes, duration, looping)
- ozz-animation under the hood — what we use, what we wrap
- Importing
  - From the editor (Import dialog)
  - From CLI (`fjimport`)
- `AnimatorComponent` — attach a skeleton + state machine to a scene node
- `BoneBuffer` — GPU side, skinning math
- Limits — max bones per draw, current restrictions
- Authoring tips — root motion, retargeting (not yet supported), naming conventions
