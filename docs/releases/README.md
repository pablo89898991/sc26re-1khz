# Release notes

Every firmware version gets a file `vNN.md` here **before** it is pushed to `master`.
It must say, in plain words, what was fixed or changed since the previous version, so anyone
(including the owner) knows exactly what they are flashing.

On push to `master`, CI builds the firmware and publishes the newest `vNN.md` as the
`vNN` GitHub release (marked Latest) with the build attached. A version is released once;
to fix the notes of an existing release, edit it on GitHub.
