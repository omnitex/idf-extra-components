# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

FTL_EVAL_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
ELF := $(FTL_EVAL_ROOT)/build/ftl_eval.elf
CONFIGS := $(wildcard $(FTL_EVAL_ROOT)/configs/*.json)

.PHONY: clean run runall

clean:
	rm -f $(FTL_EVAL_ROOT)/report_*.json $(FTL_EVAL_ROOT)/report.json

run:
	@if [ -z "$(CONFIG)" ]; then \
		echo "Usage: make run CONFIG=<name>   (e.g. CONFIG=page_relief)" >&2; \
		exit 1; \
	fi
	@if [ ! -f "$(ELF)" ]; then \
		echo "Missing $(ELF). Run: idf.py build (from $(FTL_EVAL_ROOT))" >&2; \
		exit 1; \
	fi
	@stem="$(CONFIG)"; stem="$${stem%.json}"; \
	cfg="$(FTL_EVAL_ROOT)/configs/$${stem}.json"; \
	if [ ! -f "$$cfg" ]; then \
		echo "No config file: $$cfg" >&2; \
		exit 1; \
	fi; \
	cd "$(FTL_EVAL_ROOT)" && ./build/ftl_eval.elf --config "configs/$${stem}.json"

runall:
	@if [ ! -f "$(ELF)" ]; then \
		echo "Missing $(ELF). Run: idf.py build (from $(FTL_EVAL_ROOT))" >&2; \
		exit 1; \
	fi
	@if [ -z "$(CONFIGS)" ]; then \
		echo "No JSON files in $(FTL_EVAL_ROOT)/configs/" >&2; \
		exit 1; \
	fi
	@set -e; cd "$(FTL_EVAL_ROOT)"; \
	for f in $(CONFIGS); do \
		echo "==> $$f"; \
		./build/ftl_eval.elf --config "$$f"; \
	done
