import Dispatch
import Foundation
import LocalAuthentication

let defaultReason = "bondage: authorize launch"
let args = Array(CommandLine.arguments.dropFirst())

func printUsage(to stream: UnsafeMutablePointer<FILE>) {
    fputs("Usage: touchid-check [reason...]\n", stream)
    fputs("\n", stream)
    fputs("Prompts for macOS device-owner authentication using Touch ID when available,\n", stream)
    fputs("falling back to the local account password when required by macOS policy.\n", stream)
}

if args.contains("-h") || args.contains("--help") {
    printUsage(to: stdout)
    exit(0)
}

let reason = args.isEmpty ? defaultReason : args.joined(separator: " ")
let context = LAContext()
var canEvaluateError: NSError?

guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &canEvaluateError) else {
    let message = canEvaluateError?.localizedDescription ?? "unknown error"
    fputs("touchid-check: authentication not available: \(message)\n", stderr)
    exit(2)
}

let semaphore = DispatchSemaphore(value: 0)
var authenticated = false
var authMessage = "authentication denied"

context.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: reason) { success, error in
    authenticated = success
    if let error = error {
        authMessage = error.localizedDescription
    }
    semaphore.signal()
}

semaphore.wait()

if authenticated {
    exit(0)
}

fputs("touchid-check: \(authMessage)\n", stderr)
exit(1)
