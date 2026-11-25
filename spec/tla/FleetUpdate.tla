---------------------------- MODULE FleetUpdate ----------------------------
(***************************************************************************)
(* aember-fleet-agent updating one container (src/updater.cpp), with power *)
(* losses (or the agent being stopped) at any step and the assignment      *)
(* changing at any time.                                                   *)
(*                                                                         *)
(* Disk: the image containerd starts (image), <image>.prev, <image>.new    *)
(* and the agent's state file (saved...); a power loss keeps the disk and    *)
(* loses the agent's memory; the agent starts again from its state file.   *)
(*                                                                         *)
(* Fixed = FALSE is the agent before this model: <image> is renamed to     *)
(* .prev and then .new to <image> (no image in between), and nothing is    *)
(* saved before the round ends. Fixed = TRUE is the agent now: .prev is a  *)
(* hard link (the image never goes away), the update in progress is saved  *)
(* before the container is stopped, and a restarted agent rolls an         *)
(* unfinished update back (recovery) before anything else.                 *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Versions,      \* every version that may be assigned
    Good,          \* the versions that stay up (the others crash)
    Initial,       \* the image the device shipped with (good)
    MaxLosses,     \* power losses / agent stops in all
    MaxChanges,    \* assignment changes in all
    MaxInterrupts, \* interrupted updates before a version counts as failed
    Fixed

None == "none"
ASSUME Good \subseteq Versions /\ Initial \in Good
ASSUME MaxLosses \in Nat /\ MaxChanges \in Nat /\ MaxInterrupts \in Nat \ {0}
ASSUME Fixed \in BOOLEAN

VARIABLES
    image, prev, new,          \* the files on disk (a version or None)
    savedVersion, savedFailed, \* the state file
    savedPending, savedInterrupts,
    version, failed,           \* the agent's memory of them
    pc, want,                  \* where the agent is, what it installs
    assigned,                  \* what the server assigns now
    losses, changes

vars == <<image, prev, new, savedVersion, savedFailed, savedPending,
          savedInterrupts, version, failed, pc, want, assigned, losses,
          changes>>

File == Versions \cup {None}
PCs == {"recover", "restore", "idle", "download", "prev", "pending", "stop",
        "swap1", "swap2", "start", "health", "rollback", "save"}

TypeOK ==
    /\ image \in File /\ prev \in File /\ new \in File
    /\ savedVersion \in File /\ version \in File
    /\ savedFailed \subseteq Versions /\ failed \subseteq Versions
    /\ savedPending \in File
    /\ savedInterrupts \in [Versions -> 0..MaxInterrupts]
    /\ pc \in PCs /\ want \in File
    /\ assigned \in Versions

Init ==
    /\ image = Initial /\ prev = None /\ new = None
    /\ savedVersion = None /\ savedFailed = {}
    /\ savedPending = None /\ savedInterrupts = [v \in Versions |-> 0]
    /\ version = None /\ failed = {}
    /\ pc = "recover" /\ want = None
    /\ assigned = Initial
    /\ losses = 0 /\ changes = 0

Disk == <<image, prev, new>>
Saved == <<savedVersion, savedFailed, savedPending, savedInterrupts>>
Env == <<assigned, losses, changes>>

(* The agent starts: LoadState. Fixed: an unfinished update is rolled back *)
(* first (its image never proved itself).                                 *)
Recover ==
    /\ pc = "recover"
    /\ version' = savedVersion /\ failed' = savedFailed
    /\ IF Fixed /\ savedPending # None
       THEN /\ pc' = "restore"
            /\ want' = savedPending
            \* counted before the image is touched: an interrupted recovery
            \* counts again (conservative)
            /\ LET n == savedInterrupts[savedPending] + 1 IN
               /\ savedInterrupts' = [savedInterrupts EXCEPT
                                       ![savedPending] = IF n > MaxInterrupts
                                                         THEN MaxInterrupts
                                                         ELSE n]
               /\ savedFailed' = IF n >= MaxInterrupts
                                 THEN savedFailed \cup {savedPending}
                                 ELSE savedFailed
       ELSE /\ pc' = "idle" /\ want' = None
            /\ UNCHANGED <<savedInterrupts, savedFailed>>
    /\ UNCHANGED <<image, prev, new, savedVersion, savedPending, Env>>

(* Stop, rename .prev back (atomic), start; then forget the update. *)
Restore ==
    /\ pc = "restore"
    /\ IF prev # None
       THEN /\ image' = prev /\ prev' = None
       ELSE UNCHANGED <<image, prev>>  \* done before the last power loss
    /\ savedPending' = None
    /\ failed' = savedFailed
    /\ pc' = "idle" /\ want' = None
    /\ UNCHANGED <<new, savedVersion, savedFailed, savedInterrupts, version,
                   Env>>

(* RunOnce: GetDesired. Nothing to do, a failed version, or start. *)
Poll ==
    /\ pc = "idle"
    /\ IF assigned = version \/ assigned \in failed
       THEN /\ pc' = "save" /\ want' = None
       ELSE /\ pc' = "download" /\ want' = assigned
    /\ UNCHANGED <<Disk, Saved, version, failed, Env>>

(* Download to .new, size and SHA-256 checked (one step). *)
Download ==
    /\ pc = "download"
    /\ new' = want
    /\ pc' = IF Fixed THEN "prev" ELSE "stop"
    /\ UNCHANGED <<image, prev, Saved, version, failed, want, Env>>

(* Fixed: .prev = a hard link of the image (link to .prev.tmp, rename). *)
LinkPrev ==
    /\ pc = "prev"
    /\ prev' = image
    /\ pc' = "pending"
    /\ UNCHANGED <<image, new, Saved, version, failed, want, Env>>

(* Fixed: the update in progress is saved before anything is stopped. *)
SavePending ==
    /\ pc = "pending"
    /\ savedPending' = want
    /\ pc' = "stop"
    /\ UNCHANGED <<Disk, savedVersion, savedFailed, savedInterrupts,
                   version, failed, want, Env>>

Stop ==
    /\ pc = "stop"
    /\ pc' = IF Fixed THEN "swap2" ELSE "swap1"
    /\ UNCHANGED <<Disk, Saved, version, failed, want, Env>>

(* Before: rename(image, image.prev). No image until swap2. If the image  *)
(* is missing the round ends with an error (and the container is started  *)
(* again, from nothing).                                                   *)
Swap1 ==
    /\ pc = "swap1"
    /\ IF image = None
       THEN /\ new' = None /\ pc' = "save" /\ UNCHANGED <<image, prev>>
       ELSE /\ prev' = image /\ image' = None /\ pc' = "swap2"
            /\ UNCHANGED new
    /\ UNCHANGED <<Saved, version, failed, want, Env>>

(* rename(.new, image): atomic. *)
Swap2 ==
    /\ pc = "swap2"
    /\ image' = new /\ new' = None
    /\ pc' = "health"
    /\ UNCHANGED <<prev, Saved, version, failed, want, Env>>

(* Start and watch it for health_seconds: a good version stays up. *)
Health ==
    /\ pc = "health"
    /\ IF want \in Good
       THEN /\ version' = want
            /\ pc' = "save"
            /\ UNCHANGED failed
       ELSE /\ pc' = "rollback"
            /\ UNCHANGED <<version, failed>>
    /\ UNCHANGED <<Disk, Saved, want, Env>>

(* Stop, rename(.prev, image), start; the version is marked failed. *)
Rollback ==
    /\ pc = "rollback"
    /\ image' = prev /\ prev' = None
    /\ failed' = failed \cup {want}
    /\ pc' = "save"
    /\ UNCHANGED <<new, Saved, version, want, Env>>

(* SaveState (and, fixed, the update is no longer pending). *)
Save ==
    /\ pc = "save"
    /\ savedVersion' = version /\ savedFailed' = failed
    /\ savedPending' = None
    /\ pc' = "idle" /\ want' = None
    /\ UNCHANGED <<Disk, savedInterrupts, version, failed, Env>>

(* The environment: power loss or the agent stopped, at any step; the    *)
(* server assigns another version.                                         *)
PowerLoss ==
    /\ losses < MaxLosses
    /\ losses' = losses + 1
    /\ pc' = "recover" /\ want' = None
    /\ UNCHANGED <<Disk, Saved, version, failed, assigned, changes>>

Assign(v) ==
    /\ changes < MaxChanges
    /\ v # assigned
    /\ assigned' = v /\ changes' = changes + 1
    /\ UNCHANGED <<Disk, Saved, version, failed, pc, want, losses>>

Agent == Recover \/ Restore \/ Poll \/ Download \/ LinkPrev \/ SavePending
         \/ Stop \/ Swap1 \/ Swap2 \/ Health \/ Rollback \/ Save

Next == Agent \/ PowerLoss \/ \E v \in Versions : Assign(v)

Spec == Init /\ [][Next]_vars /\ WF_vars(Agent)

---------------------------------------------------------------------------
(* Safety *)

(* containerd always has an image to start. *)
ImageAlwaysThere == image # None

(* A version that stayed up is always on disk (image or .prev). *)
GoodKept == image \in Good \/ prev \in Good

(* A version recorded as installed is the image (when the agent is idle). *)
RecordedIsImage ==
    (pc = "idle" /\ savedVersion # None /\ savedPending = None)
        => savedVersion = image

---------------------------------------------------------------------------
(* Liveness *)

(* Once the assignment and the power stay put, the device ends up at the  *)
(* assigned version, or (bad or failed) on a good one that it keeps.       *)
Settles ==
    <>[](image = assigned \/ (assigned \in savedFailed /\ image \in Good))

(* A crashing version never stays. *)
BadNeverStays == <>[](image \in Good)

=============================================================================
