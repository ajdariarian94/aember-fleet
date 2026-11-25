-------------------------- MODULE MCFleetUpdate --------------------------
(* The instance TLC checks: v1 shipped with the device, v2 good, v3 crashes. *)
EXTENDS FleetUpdate
MCVersions == {"v1", "v2", "v3"}
MCGood == {"v1", "v2"}
=============================================================================
