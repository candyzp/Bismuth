Rename-Item -Path "build\iliashdz.kyanite.geode" -NewName "iliashdz.kyanite.zip"
Compress-Archive -Path "build\iliashdz.kyanite.pdb" -Update -DestinationPath "build\iliashdz.kyanite.zip"
Rename-Item -Path "build\iliashdz.kyanite.zip" -NewName "iliashdz.kyanite.geode"