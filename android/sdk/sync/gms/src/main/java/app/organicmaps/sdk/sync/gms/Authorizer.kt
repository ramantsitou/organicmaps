package app.organicmaps.sdk.sync.gms

import android.content.Intent
import app.organicmaps.sdk.sync.AuthResult

/** Google authorization for one account at a time; [DriveAuthorizer] talks to Google Play services. */
interface Authorizer : TokenProvider {
    /** The synced account, kept across restarts; null until it is known. */
    var accountEmail: String?

    /** Authorizes [email], or lets Google Play services pick an account when it is null. */
    suspend fun authorize(email: String? = accountEmail): AuthResult

    fun resultFromIntent(data: Intent?): AuthResult

    suspend fun revoke(email: String)
}
