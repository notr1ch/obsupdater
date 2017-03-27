obsupdater
==========

Update module for Open Broadcaster Software (Classic version). Should not be used as a base for anything without first studying how OBS Classic invokes the updater. There is a known race condition between when update manifests are downloaded and verified by OBS and when this standalone updater is launched. This is not something I plan to address as if you already have untrusted code running on Windows there is not much that can be done.
