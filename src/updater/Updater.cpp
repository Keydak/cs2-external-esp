#include "Updater.hpp"

bool Updater::Init() {
	return GetInstance().InitImpl();
}

bool Updater::Process() {
	return GetInstance().ProcessImpl();
}

Status Updater::GetStatus() {
	return GetInstance().status;
}

// Reads .github/status.json of this fork. It only tells: without internet or with a broken file the program just runs.
// Whether the program still fits the game is checked against the game itself (Dumper), not here
bool Updater::InitImpl() {
	// Checked again on Retry of the loader
	status = {};
	isSetup = false;

	json response;
	auto http_status = HttpHelper::Get(this->status_url, response);

	if (http_status != 200) {
		LOGF(WARNING, "Could not check for a newer version (status {}), carrying on", http_status);
		return false;
	}

	try {
		status.unsafe = response.value("unsafe", false);
		status.notice = response.value("notice", "");

		status.version_current = response["version"].value("current", 0);
		status.version_minimum = response["version"].value("minimum", 0);
	}
	catch (std::exception& e) {
		LOGF(WARNING, "Could not read the version file ({}), carrying on", e.what());
		return false;
	}

	isSetup = true;

	// A newer version goes in front of the notice, the menu shows it on top
	if (status.version_current > current_version) {
		auto newer = std::format("Version {} is available (this is {}): {}", status.version_current, current_version, project_url);
		status.notice = status.notice.empty() ? newer : newer + "\n" + status.notice;
	}

	if (status.unsafe)
		LOGF(WARNING, "This version has been marked as \"Unsafe\" to use, its not recommended to proceed");
	else if (current_version < status.version_minimum)
		LOGF(WARNING, "This version is \"Out Of Date\" (minimum is {}), it might not work as expected", status.version_minimum);
	else if (status.version_current > current_version)
		LOGF(WARNING, "A newer version is available, this is {} and the newest is {}", current_version, status.version_current);

	if (!status.notice.empty())
		LOGF(INFO, "Notice: {}", status.notice);

	LOGF(INFO, "Successfully initialized updater (version {})...", current_version);
	return true;
}

// False when the user chose to close after a warning
bool Updater::ProcessImpl() {
	if (!isSetup)
		return true;

	auto ask = [&](const std::string& text, const char* title) {
		auto message = text + "\nDo you want to continue?\n\nMore information available at: " + project_url + "\n\nCtrl+C To copy this message";
		return MessageBox(NULL, message.c_str(), title, MB_ICONWARNING | MB_YESNO) == IDYES;
	};

	if (status.unsafe) {
		std::string text = "This version has been marked as \"Unsafe\" to use, its not recommended to proceed.";
		if (!status.notice.empty())
			text += "\n\n" + status.notice;

		if (!ask(text, "Unsafe | Warning")) {
			LOGF(VERBOSE, "Closed by the user after the \"Unsafe\" warning");
			return false;
		}

		LOGF(WARNING, "The user carried on after the \"Unsafe\" warning");
		return true;
	}

	if (current_version < status.version_minimum) {
		auto text = std::format("This version ({}) is \"out-of-date\", the minimum is {}. It might not work as expected.",
			current_version, status.version_minimum);

		if (!ask(text, "Out Of Date | Warning")) {
			LOGF(VERBOSE, "Closed by the user after the \"Out Of Date\" warning");
			return false;
		}

		LOGF(WARNING, "The user carried on after the \"Out Of Date\" warning");
		return true;
	}

	return true;
}
