#include "graphics/host_gpu/renderer/pipeline/pipelineTuning.h"

#include <iostream>
#include <string_view>

int main() {
	using namespace Libs::Graphics::PipelineTuning;
	int failures = 0;
	const auto check = [&](bool success, std::string_view name) {
		if (!success) {
			std::cerr << "FAIL: " << name << '\n';
			++failures;
		}
	};
	check(WorkerCount("", 20) == 4, "14600KF automatic policy");
	check(WorkerCount("0", 20) == 4, "zero selects automatic");
	check(WorkerCount("", 0) == 1, "unknown topology fallback");
	check(WorkerCount("", 8) == 2, "automatic policy on smaller CPUs");
	check(WorkerCount("2", 20) == 2, "two workers");
	check(WorkerCount("6", 20) == 6, "six workers");
	check(WorkerCount("99", 20) == 8, "worker cap");
	check(WorkerCount("-1", 20) == 4, "negative value fallback");
	check(WorkerCount("2junk", 20) == 4, "reject trailing characters");
	check(WorkerCount(std::string_view {}, 20) == 4, "empty string view fallback");
	check(WorkerCount("999999999999999999999", 8) == 2, "overflow fallback");
	check(WaitMilliseconds("") == 20, "original wait default");
	check(WaitMilliseconds("0") == 0, "zero wait supported");
	check(WaitMilliseconds("5") == 5, "five millisecond budget");
	check(WaitMilliseconds("9999") == 100, "wait cap");
	check(WaitMilliseconds("-1") == 20, "negative wait fallback");
	check(WaitMilliseconds("5ms") == 20, "reject partial wait parse");
	check(WaitMilliseconds("999999999999999999999") == 20, "wait overflow fallback");
	if (failures == 0) {
		std::cout << "Pipeline tuning settings: all checks passed\n";
	}
	return failures == 0 ? 0 : 1;
}
