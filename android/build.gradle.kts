import org.gradle.api.publish.maven.MavenPublication

plugins {
    id("com.android.library")
    id("maven-publish")
}

group = "com.github.github"
version = "0.29.0.gfm.13"

android {
    namespace = "org.commonmark.cmarkgfm"
    compileSdk = 36

    defaultConfig {
        minSdk = 21

        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=none")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        prefabPublishing = true
    }

    prefab {
        create("cmark_gfm") {
            headers = "src/main/prefab/cmark_gfm/include"
        }
    }

    publishing {
        singleVariant("release")
    }
}

afterEvaluate {
    publishing {
        publications {
            create<MavenPublication>("release") {
                from(components["release"])

                groupId = project.group.toString()
                artifactId = "cmark-gfm-android"
                version = project.version.toString()

                pom {
                    name.set("cmark-gfm Android")
                    description.set("Android AAR packaging for cmark-gfm with Prefab native headers and library.")
                    url.set("https://github.com/github/cmark-gfm")
                    licenses {
                        license {
                            name.set("BSD-2-Clause")
                            url.set("https://github.com/github/cmark-gfm/blob/master/COPYING")
                        }
                    }
                    scm {
                        connection.set("scm:git:https://github.com/github/cmark-gfm.git")
                        developerConnection.set("scm:git:ssh://git@github.com/github/cmark-gfm.git")
                        url.set("https://github.com/github/cmark-gfm")
                    }
                }
            }
        }
    }
}
