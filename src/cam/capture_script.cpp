/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2022, Ideas on Board Oy
 *
 * capture_script.cpp - Capture session configuration script
 */

#include "capture_script.h"

#include <iostream>
#include <stdio.h>
#include <stdlib.h>

using namespace libcamera;

CaptureScript::CaptureScript(std::shared_ptr<Camera> camera,
			     const std::string &fileName)
	: camera_(camera), valid_(false)
{
	FILE *fh = fopen(fileName.c_str(), "r");
	if (!fh) {
		int ret = -errno;
		std::cerr << "Failed to open capture script " << fileName
			  << ": " << strerror(-ret) << std::endl;
		return;
	}

	/*
	 * Map the camera's controls to their name so that they can be
	 * easily identified when parsing the script file.
	 */
	for (const auto &[control, info] : camera_->controls())
		controls_[control->name()] = control;

	int ret = parseScript(fh);
	fclose(fh);
	if (ret)
		return;

	valid_ = true;
}

/* Retrieve the control list associated with a frame number. */
const ControlList &CaptureScript::frameControls(unsigned int frame)
{
	static ControlList controls{};

	auto it = frameControls_.find(frame);
	if (it == frameControls_.end())
		return controls;

	return it->second;
}

CaptureScript::EventPtr CaptureScript::nextEvent(yaml_event_type_t expectedType)
{
	EventPtr event(new yaml_event_t);

	if (!yaml_parser_parse(&parser_, event.get()))
		return nullptr;

	if (expectedType != YAML_NO_EVENT && !checkEvent(event, expectedType))
		return nullptr;

	return event;
}

bool CaptureScript::checkEvent(const EventPtr &event, yaml_event_type_t expectedType) const
{
	if (event->type != expectedType) {
		std::cerr << "Capture script error on line " << event->start_mark.line
			  << " column " << event->start_mark.column << ": "
			  << "Expected " << eventTypeName(expectedType)
			  << " event, got " << eventTypeName(event->type)
			  << std::endl;
		return false;
	}

	return true;
}

std::string CaptureScript::eventScalarValue(const EventPtr &event)
{
	return std::string(reinterpret_cast<char *>(event->data.scalar.value),
			   event->data.scalar.length);
}

std::string CaptureScript::eventTypeName(yaml_event_type_t type)
{
	static const std::map<yaml_event_type_t, std::string> typeNames = {
		{ YAML_STREAM_START_EVENT, "stream-start" },
		{ YAML_STREAM_END_EVENT, "stream-end" },
		{ YAML_DOCUMENT_START_EVENT, "document-start" },
		{ YAML_DOCUMENT_END_EVENT, "document-end" },
		{ YAML_ALIAS_EVENT, "alias" },
		{ YAML_SCALAR_EVENT, "scalar" },
		{ YAML_SEQUENCE_START_EVENT, "sequence-start" },
		{ YAML_SEQUENCE_END_EVENT, "sequence-end" },
		{ YAML_MAPPING_START_EVENT, "mapping-start" },
		{ YAML_MAPPING_END_EVENT, "mapping-end" },
	};

	auto it = typeNames.find(type);
	if (it == typeNames.end())
		return "[type " + std::to_string(type) + "]";

	return it->second;
}

int CaptureScript::parseScript(FILE *script)
{
	int ret = yaml_parser_initialize(&parser_);
	if (!ret) {
		std::cerr << "Failed to initialize yaml parser" << std::endl;
		return ret;
	}

	/* Delete the parser upon function exit. */
	struct ParserDeleter {
		ParserDeleter(yaml_parser_t *parser) : parser_(parser) { }
		~ParserDeleter() { yaml_parser_delete(parser_); }
		yaml_parser_t *parser_;
	} deleter(&parser_);

	yaml_parser_set_input_file(&parser_, script);

	EventPtr event = nextEvent(YAML_STREAM_START_EVENT);
	if (!event)
		return -EINVAL;

	event = nextEvent(YAML_DOCUMENT_START_EVENT);
	if (!event)
		return -EINVAL;

	event = nextEvent(YAML_MAPPING_START_EVENT);
	if (!event)
		return -EINVAL;

	while (1) {
		event = nextEvent();
		if (!event)
			return -EINVAL;

		if (event->type == YAML_MAPPING_END_EVENT)
			return 0;

		if (!checkEvent(event, YAML_SCALAR_EVENT))
			return -EINVAL;

		std::string section = eventScalarValue(event);

		if (section == "frames") {
			parseFrames();
		} else {
			std::cerr << "Unsupported section '" << section << "'"
				  << std::endl;
			return -EINVAL;
		}
	}
}

int CaptureScript::parseFrames()
{
	EventPtr event = nextEvent(YAML_SEQUENCE_START_EVENT);
	if (!event)
		return -EINVAL;

	while (1) {
		event = nextEvent();
		if (!event)
			return -EINVAL;

		if (event->type == YAML_SEQUENCE_END_EVENT)
			return 0;

		int ret = parseFrame(std::move(event));
		if (ret)
			return ret;
	}
}

int CaptureScript::parseFrame(EventPtr event)
{
	if (!checkEvent(event, YAML_MAPPING_START_EVENT))
		return -EINVAL;

	std::string key = parseScalar();
	if (key.empty())
		return -EINVAL;

	unsigned int frameId = atoi(key.c_str());

	event = nextEvent(YAML_MAPPING_START_EVENT);
	if (!event)
		return -EINVAL;

	ControlList controls{};

	while (1) {
		event = nextEvent();
		if (!event)
			return -EINVAL;

		if (event->type == YAML_MAPPING_END_EVENT)
			break;

		int ret = parseControl(std::move(event), controls);
		if (ret)
			return ret;
	}

	frameControls_[frameId] = std::move(controls);

	event = nextEvent(YAML_MAPPING_END_EVENT);
	if (!event)
		return -EINVAL;

	return 0;
}

int CaptureScript::parseControl(EventPtr event, ControlList &controls)
{
	/* We expect a value after a key. */
	std::string name = eventScalarValue(event);
	if (name.empty())
		return -EINVAL;

	/* If the camera does not support the control just ignore it. */
	auto it = controls_.find(name);
	if (it == controls_.end()) {
		std::cerr << "Unsupported control '" << name << "'" << std::endl;
		return -EINVAL;
	}

	std::string value = parseScalar();
	if (value.empty())
		return -EINVAL;

	const ControlId *controlId = it->second;
	ControlValue val = unpackControl(controlId, value);
	controls.set(controlId->id(), val);

	return 0;
}

std::string CaptureScript::parseScalar()
{
	EventPtr event = nextEvent(YAML_SCALAR_EVENT);
	if (!event)
		return "";

	return eventScalarValue(event);
}

void CaptureScript::unpackFailure(const ControlId *id, const std::string &repr)
{
	static const std::map<unsigned int, const char *> typeNames = {
		{ ControlTypeNone, "none" },
		{ ControlTypeBool, "bool" },
		{ ControlTypeByte, "byte" },
		{ ControlTypeInteger32, "int32" },
		{ ControlTypeInteger64, "int64" },
		{ ControlTypeFloat, "float" },
		{ ControlTypeString, "string" },
		{ ControlTypeRectangle, "Rectangle" },
		{ ControlTypeSize, "Size" },
	};

	const char *typeName;
	auto it = typeNames.find(id->type());
	if (it != typeNames.end())
		typeName = it->second;
	else
		typeName = "unknown";

	std::cerr << "Unsupported control '" << repr << "' for "
		  << typeName << " control " << id->name() << std::endl;
}

ControlValue CaptureScript::unpackControl(const ControlId *id,
					  const std::string &repr)
{
	ControlValue value{};

	switch (id->type()) {
	case ControlTypeNone:
		break;
	case ControlTypeBool: {
		bool val;

		if (repr == "true") {
			val = true;
		} else if (repr == "false") {
			val = false;
		} else {
			unpackFailure(id, repr);
			return value;
		}

		value.set<bool>(val);
		break;
	}
	case ControlTypeByte: {
		uint8_t val = strtol(repr.c_str(), NULL, 10);
		value.set<uint8_t>(val);
		break;
	}
	case ControlTypeInteger32: {
		int32_t val = strtol(repr.c_str(), NULL, 10);
		value.set<int32_t>(val);
		break;
	}
	case ControlTypeInteger64: {
		int64_t val = strtoll(repr.c_str(), NULL, 10);
		value.set<int64_t>(val);
		break;
	}
	case ControlTypeFloat: {
		float val = strtof(repr.c_str(), NULL);
		value.set<float>(val);
		break;
	}
	case ControlTypeString: {
		value.set<std::string>(repr);
		break;
	}
	case ControlTypeRectangle:
		/* \todo Parse rectangles. */
		break;
	case ControlTypeSize:
		/* \todo Parse Sizes. */
		break;
	}

	return value;
}