// swift-tools-version:5.9
// Foundation-only logic for the VoidHealth app: the watch's record format,
// the BLE history protocol, the on-phone store and every derived metric.
// No UIKit / SwiftUI / CoreBluetooth here, so `swift test` runs anywhere.
import PackageDescription

let package = Package(
    name: "VoidHealthCore",
    platforms: [.iOS(.v17), .macOS(.v14)],
    products: [
        .library(name: "VoidHealthCore", targets: ["VoidHealthCore"]),
    ],
    targets: [
        .target(name: "VoidHealthCore"),
        .testTarget(name: "VoidHealthCoreTests", dependencies: ["VoidHealthCore"]),
    ]
)
