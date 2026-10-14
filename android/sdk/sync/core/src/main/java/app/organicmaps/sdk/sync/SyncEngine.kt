package app.organicmaps.sdk.sync

import app.organicmaps.sdk.sync.SyncAction.ConflictCopy
import app.organicmaps.sdk.sync.SyncAction.Download
import app.organicmaps.sdk.sync.SyncAction.DropBase
import app.organicmaps.sdk.sync.SyncAction.RecordBase
import app.organicmaps.sdk.sync.SyncAction.ResolveDuplicate
import app.organicmaps.sdk.sync.SyncAction.TrashCloud
import app.organicmaps.sdk.sync.SyncAction.TrashLocal
import app.organicmaps.sdk.sync.SyncAction.Upload
import java.io.File
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

sealed interface SyncStatus {
    data class Idle(val lastSync: Long?) : SyncStatus

    /** [lastSync] stays on screen while the pass runs. */
    data class Running(val lastSync: Long?) : SyncStatus

    data object NeedsSignIn : SyncStatus

    data class Error(val error: SyncError) : SyncStatus
}

sealed interface PassResult {
    data object Synced : PassResult

    /** Some remote changes wait until no screen keeps categories from being replaced. */
    data object Deferred : PassResult

    data class Failed(val error: SyncError) : PassResult
}

/** Runs one reconciliation pass at a time and persists the base state after every applied action. */
class SyncEngine(
    private val cloud: CloudStore,
    private val local: LocalStore,
    private val baseStore: BaseStateStore,
    private val deviceName: String,
    private val now: () -> Long,
) {
    private val mutex = Mutex()
    private val mutableStatus = MutableStateFlow<SyncStatus>(SyncStatus.Idle(baseStore.load().lastSync))
    val status: StateFlow<SyncStatus> = mutableStatus.asStateFlow()

    /** True during a pass, and while the cloud data is being deleted. */
    val isRunning: Boolean
        get() = mutex.isLocked

    /** Shows a failure that happened outside of a pass, like a background authorization. */
    fun showError(error: SyncError) {
        mutableStatus.value = statusFor(error)
    }

    /** Clears a sign-in request or an error shown before; the next pass reports its own result. */
    fun onAuthorized() {
        val status = mutableStatus.value
        if (status == SyncStatus.NeedsSignIn || status is SyncStatus.Error) {
            mutableStatus.value = SyncStatus.Idle(baseStore.load().lastSync)
        }
    }

    @Suppress("TooGenericExceptionCaught") // Any failure must end the pass with a visible status, not a running one.
    suspend fun runPass(): PassResult = mutex.withLock {
        mutableStatus.value = SyncStatus.Running(baseStore.load().lastSync)
        try {
            val result = pass()
            mutableStatus.value = SyncStatus.Idle(baseStore.load().lastSync)
            result
        } catch (e: CloudException) {
            mutableStatus.value = statusFor(e.error)
            PassResult.Failed(e.error)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            val error = SyncError.Other(e.message ?: e.javaClass.simpleName)
            mutableStatus.value = SyncStatus.Error(error)
            PassResult.Failed(error)
        } finally {
            if (mutableStatus.value is SyncStatus.Running) {
                mutableStatus.value = SyncStatus.Idle(baseStore.load().lastSync)
            }
        }
    }

    /** Forgets the base first, so a partial deletion can never look like files deleted on this device. */
    suspend fun deleteCloudData() = mutex.withLock {
        clearBase()
        withContext(NonCancellable) { cloud.deleteAll() }
    }

    /** Waits for a running pass, which would otherwise save the base again. */
    suspend fun forgetLocalState() = mutex.withLock { clearBase() }

    private fun statusFor(error: SyncError): SyncStatus = when (error) {
        SyncError.AuthRequired -> SyncStatus.NeedsSignIn

        // WorkManager retries, so the last sync stays on screen.
        SyncError.Transient -> SyncStatus.Idle(baseStore.load().lastSync)

        else -> SyncStatus.Error(error)
    }

    private fun clearBase() {
        baseStore.delete()
        mutableStatus.value = SyncStatus.Idle(null)
    }

    private suspend fun pass(): PassResult {
        local.cleanTemp()
        val listing = cloud.list()
        val generation = listing.generation ?: cloud.createGeneration()
        val localFiles = local.snapshot()
        var state = baseStore.load()
        // Another account, or all data deleted from another device or in the Drive settings: the cloud files share
        // no history with the base, so a missing one must not turn into a local deletion.
        if (state.generation != generation) state = BaseState.EMPTY.copy(generation = generation)
        baseStore.save(state)

        val cloudNames = listing.files.mapTo(HashSet()) { it.name }
        var deferred = false
        for (action in Reconciler.plan(localFiles, listing.files, state.files)) {
            if (changesLocalFiles(action) && !local.canApplyChanges()) {
                deferred = true
                continue
            }
            state = apply(action, state, cloudNames)
            baseStore.save(state)
        }

        state.trashFolderId?.let { cloud.purgeTrash(it, now() - TRASH_RETENTION_MILLIS) }
        if (deferred) return PassResult.Deferred
        // Shown as the last sync, so it marks only passes that left nothing for later.
        baseStore.save(state.copy(lastSync = now()))
        return PassResult.Synced
    }

    private fun changesLocalFiles(action: SyncAction) = when (action) {
        is Download, is ConflictCopy, is TrashLocal -> true
        is ResolveDuplicate -> action.keepAsLocalCopy
        else -> false
    }

    private suspend fun apply(action: SyncAction, state: BaseState, cloudNames: Set<String>): BaseState =
        when (action) {
            is Upload -> upload(action, state)

            is Download -> {
                val installed = downloadVerified(action.cloud)?.let {
                    local.install(action.name, it, action.expectedLocalMd5)
                }
                if (installed == true) state.withFile(action.name, action.cloud) else state
            }

            is ConflictCopy -> conflictCopy(action, state, cloudNames)

            is TrashCloud -> trashCloud(action, state)

            is TrashLocal -> trashLocal(action, state)

            is RecordBase -> state.copy(files = state.files + (action.name to action.entry))

            is DropBase -> state.withoutFile(action.name)

            is ResolveDuplicate -> resolveDuplicate(action, state, cloudNames)
        }

    private suspend fun upload(action: Upload, state: BaseState): BaseState {
        // Looked up by id, as a listing can lag behind and miss a file that still exists.
        val currentMd5 = action.cloudId?.let { cloud.currentMd5(it) }
        // Overwrite only the version unchanged since the last sync; a file that is gone is created again.
        if (currentMd5 != null && currentMd5 != state.files[action.name]?.md5) return state
        val existingId = if (currentMd5 == null) null else action.cloudId
        return state.withFile(action.name, cloud.upload(action.name, local.file(action.name), existingId))
    }

    private suspend fun trashLocal(action: TrashLocal, state: BaseState): BaseState {
        // A listing that lags behind must not delete a list: trash it only once the cloud file is really gone.
        if (cloud.currentMd5(action.cloudId) != null) return state
        return if (local.trash(action.name, action.expectedMd5)) state.withoutFile(action.name) else state
    }

    private suspend fun conflictCopy(action: ConflictCopy, state: BaseState, cloudNames: Set<String>): BaseState {
        val tmp = downloadVerified(action.cloud) ?: return state
        if (!hasPlacemarks(tmp)) {
            // An untouched default list from another device must not push this device's list into a copy.
            tmp.delete()
            if (cloud.currentMd5(action.cloud.id) != action.cloud.md5) return state
            return state.withFile(action.name, cloud.upload(action.name, local.file(action.name), action.cloud.id))
        }
        local.resolveConflict(action.name, tmp, action.localMd5, deviceName, cloudNames) ?: return state
        return state.withFile(action.name, action.cloud)
    }

    private suspend fun trashCloud(action: TrashCloud, state: BaseState): BaseState =
        when (cloud.currentMd5(action.cloudId)) {
            null -> state.withoutFile(action.name)

            action.expectedMd5 -> {
                val trashFolderId = cloud.moveToTrash(action.cloudId, state.trashFolderId)
                state.copy(trashFolderId = trashFolderId).withoutFile(action.name)
            }

            else -> state
        }

    private suspend fun resolveDuplicate(
        action: ResolveDuplicate,
        state: BaseState,
        cloudNames: Set<String>,
    ): BaseState {
        if (action.keepAsLocalCopy) {
            val tmp = downloadVerified(action.duplicate) ?: return state
            if (hasPlacemarks(tmp)) {
                local.installUnique(action.name, tmp, cloudNames) ?: return state
            } else {
                tmp.delete()
            }
        }
        return state.copy(trashFolderId = cloud.moveToTrash(action.duplicate.id, state.trashFolderId))
    }

    /** @return null if the file changed or was deleted after it was listed; the next pass sees the new state. */
    private suspend fun downloadVerified(file: CloudFile): File? {
        val tmp = local.newTempFile()
        if (cloud.download(file.id, tmp) && Md5.of(tmp) == file.md5) return tmp
        tmp.delete()
        return null
    }

    /** False for a category without bookmarks and tracks, like an untouched default list. */
    private fun hasPlacemarks(file: File): Boolean =
        file.bufferedReader().useLines { lines -> lines.any { "<Placemark" in it } }

    private fun BaseState.withFile(name: String, file: CloudFile) = copy(
        files =
            files + (name to BaseEntry(file.id, file.md5)),
    )

    private fun BaseState.withoutFile(name: String) = copy(files = files - name)

    private companion object {
        const val TRASH_RETENTION_MILLIS = 30L * 24 * 60 * 60 * 1000
    }
}
