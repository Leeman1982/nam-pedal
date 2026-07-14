Import("env")
import os

project_dir = env["PROJECT_DIR"]
nam_dir = os.path.normpath(os.path.join(project_dir, "..", "NeuralAmpModelerCore", "NAM"))
namb_dir = os.path.normpath(os.path.join(project_dir, "..", "nam-binary-loader", "namb"))

env.BuildSources(
    os.path.join("$BUILD_DIR", "NAM"),
    nam_dir,
    src_filter=["+<*.cpp>"]
)

env.BuildSources(
    os.path.join("$BUILD_DIR", "namb"),
    namb_dir,
    src_filter=["+<*.cpp>"]
)
