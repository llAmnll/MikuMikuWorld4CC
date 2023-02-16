#include "ScoreEditor.h"
#include "ResourceManager.h"
#include "HistoryManager.h"
#include "Rendering/Renderer.h"
#include "Score.h"
#include "SUSIO.h"
#include "Utilities.h"
#include "IconsFontAwesome5.h"
#include "Colors.h"
#include "InputListener.h"
#include "UI.h"
#include "FileDialog.h"
#include "StringOperations.h"
#include "Localization.h"
#include <tinyfiledialogs.h>
#include <algorithm>

namespace MikuMikuWorld
{
	ScoreEditor::ScoreEditor() : score{}, prevUpdateScore{},
		isHoveringNote{ false }, isHoldingNote{ false }, isMovingNote{ false }, currentMode{ 0 },
		dummy{ NoteType::Tap }, dummyStart{ NoteType::Hold }, dummyMid{ NoteType::HoldMid }, dummyEnd{ NoteType::HoldEnd }
	{
		drawHoldStepOutline = true;
		showRenderStats = true;
		mouseClickedOnTimeline = false;

		defaultNoteWidth = 3;
		defaultStepType = HoldStepType::Visible;
		defaultBPM = 160.0f;
		defaultTimeSignN = defaultTimeSignD = 4;

		skipUpdateAfterSortingSteps = false;
		framebuffer = new Framebuffer(1080, 1920);

		time = 0;
		playStartTime = 0;
		playing = false;
		dragging = false;
		hasEdit = false;
		pasting = flipPasting = insertingPreset = false;
		uptoDate = true;

		audio.initAudio();
	}

	ScoreEditor::~ScoreEditor()
	{
		audio.uninitAudio();
	}

	void ScoreEditor::readScoreMetadata()
	{
		workingData.title = score.metadata.title;
		workingData.designer = score.metadata.author;
		workingData.artist = score.metadata.artist;
		workingData.musicOffset = score.metadata.musicOffset;
		workingData.jacket.load(score.metadata.jacketFile);

		loadMusic(score.metadata.musicFile);
		audio.setBGMOffset(time, workingData.musicOffset);
	}

	void ScoreEditor::writeScoreMetadata()
	{
		score.metadata.title = workingData.title;
		score.metadata.author = workingData.designer;
		score.metadata.artist = workingData.artist;
		score.metadata.musicFile = workingData.musicFilename;
		score.metadata.musicOffset = workingData.musicOffset;
		score.metadata.jacketFile = workingData.jacket.getFilename();
	}

	void ScoreEditor::loadScore(const std::string& filename)
	{
		if (playing)
			togglePlaying();

		std::string extension = File::getFileExtension(filename);
		std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
		std::string title = windowUntitled;

		int nextIdBackup = nextID;
		try
		{
			resetNextID();
			if (extension == SUS_EXTENSION)
			{
				SUSIO susIO;
				score = susIO.importSUS(filename);
				workingData.filename = "";

				// project not saved
				uptoDate = false;
			}
			else if (extension == MMWS_EXTENSION)
			{
				score = deserializeScore(filename);
				uptoDate = true;

				workingData.filename = filename;
				title = File::getFilenameWithoutExtension(filename);
			}

			selection.clear();
			history.clear();
			hasEdit = false;

			readScoreMetadata();
			stats.calculateStats(score);
			UI::setWindowTitle(title);
		}
		catch (std::runtime_error& err)
		{
			nextID = nextIdBackup;

			std::string errMsg = "An error occured while reading the score file.\n" + std::string(err.what());
			tinyfd_messageBox(APP_NAME, errMsg.c_str(), "ok", "error", 1);
		}
	}

	void ScoreEditor::loadMusic(const std::string& filename)
	{
		audio.changeBGM(filename);
		workingData.musicFilename = filename;
	}

	void ScoreEditor::open()
	{
		std::string filename;
		if (FileDialog::openFile(filename, FileType::ScoreFile))
			loadScore(filename);
	}

	void ScoreEditor::save()
	{
		if (workingData.filename.size())
		{
			writeScoreMetadata();
			serializeScore(score, workingData.filename);
			uptoDate = true;
			UI::setWindowTitle(File::getFilenameWithoutExtension(workingData.filename));
		}
		else
		{
			saveAs();
		}
	}

	void ScoreEditor::save(const std::string& filename)
	{
		writeScoreMetadata();
		serializeScore(score, filename);
	}

	void ScoreEditor::saveAs()
	{
		std::string filename;
		if (FileDialog::saveFile(filename, FileType::MMWSFile))
		{
			workingData.filename = filename;
			writeScoreMetadata();
			serializeScore(score, filename);
			uptoDate = true;

			UI::setWindowTitle(File::getFilenameWithoutExtension(workingData.filename));
		}
	}

	void ScoreEditor::exportSUS()
	{
		std::string filename;
		if (FileDialog::saveFile(filename, FileType::SUSFile))
		{
			writeScoreMetadata();
			SUSIO susIO;
			susIO.exportSUS(score, filename);
		}
	}

	void ScoreEditor::reset()
	{
		resetEditor();
		audio.disposeBGM();
		UI::setWindowTitle(windowUntitled);
	}

	void ScoreEditor::resetEditor()
	{
		playing = false;
		audio.stopSounds(false);
		audio.stopBGM();

		selection.clear();
		history.clear();
		resetNextID();

		workingData = EditorScoreData{};
		score = Score{};
		stats.reset();

		hasEdit = false;
		uptoDate = true;
	}

	bool ScoreEditor::isUptoDate() const
	{
		return uptoDate;
	}

	int ScoreEditor::snapTickFromPos(float posY)
	{
		return snapTick(canvas.positionToTick(posY), division);
	}

	int ScoreEditor::snapTick(int tick, int div)
	{
		int half = (TICKS_PER_BEAT / (div / 4)) / 2;
		int remaining = tick % (TICKS_PER_BEAT / (div / 4));

		// round to closest division
		tick -= remaining;
		if (remaining >= half)
			tick += half * 2;

		return std::max(tick, 0);
	}

	int ScoreEditor::roundTickDown(int tick, int div)
	{
		return std::max(tick - (tick % (TICKS_PER_BEAT / (div / 4))), 0);
	}

	int ScoreEditor::laneFromCenterPos(int lane, int width)
	{
		return std::clamp(lane - (width / 2), MIN_LANE, MAX_LANE - width + 1);
	}

	void ScoreEditor::togglePlaying()
	{
		playing ^= true;
		if (playing)
		{
			playStartTime = time;
			audio.seekBGM(time);
			audio.reSync();
			audio.playBGM(time);
		}
		else
		{
			audio.stopSounds(false);
			audio.stopBGM();
		}
	}

	void ScoreEditor::stop()
	{
		playing = false;
		time = currentTick = 0;

		canvas.scrollToBeginning();
		audio.stopSounds(false);
		audio.stopBGM();
	}

	void ScoreEditor::stopAtLastSelectedTick()
	{
		if (playing)
		{
			playing = false;
			audio.stopSounds(false);
			audio.stopBGM();
			currentTick = lastSelectedTick;
			canvas.centerCursor(currentTick, false, 0);
		}
		else
		{
			togglePlaying();
		}
	}

	void ScoreEditor::restart()
	{
		stop();
		togglePlaying();
	}

	bool ScoreEditor::isPlaying()
	{
		return playing;
	}

	void ScoreEditor::nextTick()
	{
		currentTick = roundTickDown(currentTick, division);
		currentTick += TICKS_PER_BEAT / (division / 4);
		canvas.centerCursor(currentTick, playing, 1);
	}

	void ScoreEditor::previousTick()
	{
		if (currentTick <= 0)
		{
			currentTick = 0;
			return;
		}

		currentTick = roundTickDown(currentTick, division);
		currentTick = std::max(currentTick - (TICKS_PER_BEAT / (division / 4)), 0);
		canvas.centerCursor(currentTick, playing, 2);
	}

	void ScoreEditor::setDivision(int div)
	{
		if (div >= 4 && div <= 1920)
			division = div;
	}

	void ScoreEditor::setScrollMode(ScrollMode mode)
	{
		scrollMode = mode;
	}

	void ScoreEditor::setScrollMode(std::string mode)
	{
		int m = 0;
		for (int i = 0; i < sizeof(scrollModes); ++i)
		{
			if (mode == scrollModes[i])
			{
				m = i;
				break;
			}
		}

		scrollMode = (ScrollMode)m;
	}

	void ScoreEditor::pushHistory(const std::string& description, const Score& prev, const Score& curr)
	{
		history.pushHistory(description, prev, curr);

		if (uptoDate)
			UI::setWindowTitle("*" + (workingData.filename.size() ? File::getFilenameWithoutExtension(workingData.filename) : windowUntitled));

		uptoDate = false;
		stats.calculateStats(score);
	}

	void ScoreEditor::undo()
	{
		if (history.hasUndo())
		{
			score = history.undo();
			clearSelection();
			uptoDate = false;

			stats.calculateStats(score);
		}
	}

	void ScoreEditor::redo()
	{
		if (history.hasRedo())
		{
			score = history.redo();
			clearSelection();
			uptoDate = false;

			stats.calculateStats(score);
		}
	}

	void ScoreEditor::gotoMeasure(int measure)
	{
		if (measure < 0 || measure > 999)
			return;

		currentTick = measureToTicks(measure, TICKS_PER_BEAT, score.timeSignatures);
		canvas.centerCursor(currentTick, playing, 0);
	}

	void ScoreEditor::updateNoteSE()
	{
		songPosLastFrame = songPos;

		if (audio.isMusicInitialized() && playing && (audio.getAudioPosition() >= (workingData.musicOffset / 1000)) && !audio.isMusicAtEnd())
			songPos = audio.getAudioPosition() + (workingData.musicOffset / 1000);
		else
			songPos = time;

		if (!playing)
			return;

		tickSEMap.clear();
		for (const auto& it : score.notes)
		{
			const Note& note = it.second;
			float noteTime = accumulateDuration(note.tick, TICKS_PER_BEAT, score.tempoChanges);
			float notePlayTime = noteTime - playStartTime;
			float offsetNoteTime = noteTime - audioLookAhead;

			if (offsetNoteTime >= songPosLastFrame && offsetNoteTime < songPos)
			{
				std::string se = getNoteSE(note, score);
				std::string key = std::to_string(note.tick) + "-" + se;
				if (se.size());
				{
					if (tickSEMap.find(key) == tickSEMap.end())
					{
						audio.playSound(se.c_str(), notePlayTime - audioOffsetCorrection, -1);
						tickSEMap[key] = note.tick;
					}
				}

				if (note.getType() == NoteType::Hold)
				{
					float endTime = accumulateDuration(score.notes.at(score.holdNotes.at(note.ID).end).tick, TICKS_PER_BEAT, score.tempoChanges);
					audio.playSound(note.critical ? SE_CRITICAL_CONNECT : SE_CONNECT, notePlayTime - audioOffsetCorrection, endTime - playStartTime - audioOffsetCorrection);
				}
			}
			else if (time == playStartTime)
			{
				// playback just started
				if (noteTime >= time && offsetNoteTime < time)
				{
					std::string se = getNoteSE(note, score);
					std::string key = std::to_string(note.tick) + "-" + se;
					if (se.size())
					{
						if (tickSEMap.find(key) == tickSEMap.end())
						{
							audio.playSound(se.c_str(), notePlayTime, -1);
							tickSEMap[key] = note.tick;
						}
					}
				}

				// playback started mid-hold
				if (note.getType() == NoteType::Hold)
				{
					int endTick = score.notes.at(score.holdNotes.at(note.ID).end).tick;
					float endTime = accumulateDuration(endTick, TICKS_PER_BEAT, score.tempoChanges);

					if ((noteTime - time) <= audioLookAhead && endTime > time)
						audio.playSound(note.critical ? SE_CRITICAL_CONNECT : SE_CONNECT, std::max(0.0f, notePlayTime), endTime - playStartTime);
				}
			}
		}
	}

	void ScoreEditor::drawMeasures()
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();

		const float x1 = canvas.getTimelineStartX();
		const float x2 = canvas.getTimelineEndX();

		int firstTick = std::max(0, canvas.positionToTick(canvas.getVisualOffset() - canvas.getSize().y));
		int lastTick = canvas.positionToTick(canvas.getVisualOffset());
		int measure = accumulateMeasures(firstTick, TICKS_PER_BEAT, score.timeSignatures);
		firstTick = measureToTicks(measure, TICKS_PER_BEAT, score.timeSignatures);

		int tsIndex = findTimeSignature(measure, score.timeSignatures);
		int subDiv = TICKS_PER_BEAT / (division < 192 ? (division / 4) : 1);
		int div = TICKS_PER_BEAT;

		for (int tick = firstTick; tick <= lastTick; tick += subDiv)
		{
			const float y = canvas.getPosition().y - canvas.tickToPosition(tick) + canvas.getVisualOffset();
			int measure = accumulateMeasures(tick, TICKS_PER_BEAT, score.timeSignatures);

			// time signature changes on current measure
			if (score.timeSignatures.find(measure) != score.timeSignatures.end())
				tsIndex = measure;

			if (!(tick % div))
				drawList->AddLine(ImVec2(x1, y), ImVec2(x2, y), divColor1, primaryLineThickness);
			else if (division < 192)
				drawList->AddLine(ImVec2(x1, y), ImVec2(x2, y), divColor2, secondaryLineThickness);
		}

		tsIndex = findTimeSignature(measure, score.timeSignatures);
		int ticksPerMeasure = beatsPerMeasure(score.timeSignatures[tsIndex]) * TICKS_PER_BEAT;

		for (int tick = firstTick; tick < lastTick; tick += ticksPerMeasure)
		{
			if (score.timeSignatures.find(measure) != score.timeSignatures.end())
			{
				tsIndex = measure;
				ticksPerMeasure = beatsPerMeasure(score.timeSignatures[tsIndex]) * TICKS_PER_BEAT;
			}

			std::string measureStr = "#" + std::to_string(measure);
			const float txtPos = x1 - MEASURE_WIDTH - (ImGui::CalcTextSize(measureStr.c_str()).x * 0.5f);
			const float y = canvas.getPosition().y - canvas.tickToPosition(tick) + canvas.getVisualOffset();

			drawList->AddLine(ImVec2(x1 - MEASURE_WIDTH, y), ImVec2(x2 + MEASURE_WIDTH, y), measureColor, 1.5f);
			drawList->AddText(ImGui::GetFont(), 26.0f, ImVec2(txtPos, y), measureTxtColor, measureStr.c_str());

			++measure;
		}
	}

	void ScoreEditor::drawLanes()
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		for (int l = 0; l <= NUM_LANES; ++l)
		{
			const float x = canvas.getPosition().x + canvas.laneToPosition(l);
			const bool boldLane = !(l & 1);
			const ImU32 color = boldLane ? divColor1 : divColor2;
			const float thickness = boldLane ? primaryLineThickness : secondaryLineThickness;
			drawList->AddLine(ImVec2(x, canvas.getPosition().y), ImVec2(x, canvas.getPosition().y + canvas.getSize().y), color, secondaryLineThickness);
		}
	}

	void ScoreEditor::updateCursor()
	{
		hoverTick = snapTickFromPos(-mousePos.y);
		hoverLane = canvas.positionToLane(mousePos.x);
		if (ImGui::IsMouseClicked(0) && !isHoveringNote && canvas.isMouseInCanvas() && !playing &&
			!UI::isAnyPopupOpen() && currentMode == TimelineMode::Select && ImGui::IsWindowFocused())
		{
			currentTick = hoverTick;
			lastSelectedTick = currentTick;
		}

		const float x1 = canvas.getTimelineStartX();
		const float x2 = canvas.getTimelineEndX();
		const float y = canvas.getPosition().y - canvas.tickToPosition(currentTick) + canvas.getVisualOffset();
		const float triPtOffset = 8.0f;
		const float triXPos = x1 - (triPtOffset * 2);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddTriangleFilled
		(
			ImVec2(triXPos, y - triPtOffset),
			ImVec2(triXPos, y + triPtOffset),
			ImVec2(triXPos + (triPtOffset * 2), y), cursorColor
		);

		drawList->AddLine(ImVec2(x1, y), ImVec2(x2, y), cursorColor, primaryLineThickness + 1.0f);
	}

	void ScoreEditor::updateTempoChanges()
	{
		for (int index = 0; index < score.tempoChanges.size(); ++index)
		{
			Tempo& tempo = score.tempoChanges[index];
			if (bpmControl(tempo))
			{
				editBPMIndex = index;
				editBPM = tempo.bpm;
				ImGui::OpenPopup("edit_bpm");
			}
		}
	}

	void ScoreEditor::updateTimeSignatures()
	{
		for (auto& [measure, ts] : score.timeSignatures)
		{
			if (timeSignatureControl(ts))
			{
				editTSIndex = measure;
				editTsNum = ts.numerator;
				editTsDenom = ts.denominator;
				ImGui::OpenPopup("edit_ts");
			}
		}
	}

	bool ScoreEditor::noteControl(const ImVec2& pos, const ImVec2& sz, const char* id, ImGuiMouseCursor cursor)
	{
		ImGui::SetCursorScreenPos(pos);
		ImGui::InvisibleButton(id, sz);
		if (ImGui::IsItemHovered())
			ImGui::SetMouseCursor(cursor);

		// note clicked
		if (ImGui::IsItemActivated())
		{
			prevUpdateScore = score;
			ctrlMousePos = mousePos;
			holdLane = hoverLane;
			holdTick = hoverTick;
		}

		// holding note
		if (ImGui::IsItemActive())
		{
			ImGui::SetMouseCursor(cursor);
			isHoldingNote = true;
			return true;
		}

		// note released
		if (ImGui::IsItemDeactivated())
		{
			isHoldingNote = false;
			if (hasEdit)
			{
				std::unordered_set<int> sortHolds = selection.getHolds(score);
				for (int id : sortHolds)
				{
					HoldNote& hold = score.holdNotes.at(id);
					Note& start = score.notes.at(id);
					Note& end = score.notes.at(hold.end);

					if (start.tick > end.tick)
					{
						std::swap(start.tick, end.tick);
						std::swap(start.lane, end.lane);
					}

					sortHoldSteps(score, hold);
					if (hold.steps.size())
					{
						// ensure hold steps are between the start and end
						Note& firstMid = score.notes.at(hold.steps[0].ID);
						if (start.tick > firstMid.tick)
						{
							std::swap(start.tick, firstMid.tick);
							std::swap(start.lane, firstMid.lane);
						}

						Note& lastMid = score.notes.at(hold.steps[hold.steps.size() - 1].ID);
						if (end.tick < lastMid.tick)
						{
							std::swap(end.tick, lastMid.tick);
							std::swap(end.lane, lastMid.lane);
						}
					}

					pushHistory("Update notes", prevUpdateScore, score);
					skipUpdateAfterSortingSteps = true;
					hasEdit = false;
				}
			}
		}

		return false;
	}

	int ScoreEditor::findClosestHold()
	{
		float xt = canvas.laneToPosition(hoverLane);
		float yt = canvas.getNoteYPosFromTick(hoverTick);

		for (auto it = score.holdNotes.begin(); it != score.holdNotes.end(); ++it)
		{
			const HoldNote& hold = it->second;
			const Note& start = score.notes.at(hold.start.ID);
			const Note& end = score.notes.at(hold.end);
			const int ID = hold.start.ID;

			if (hold.steps.size())
			{
				const HoldStep& mid1 = hold.steps[0];
				if (isHoldPathInTick(start, score.notes.at(mid1.ID), hold.start.ease, xt, yt))
					return ID;

				for (int step = 0; step < hold.steps.size() - 1; ++step)
				{
					const Note& m1 = score.notes.at(hold.steps[step].ID);
					const Note& m2 = score.notes.at(hold.steps[step + 1].ID);
					if (isHoldPathInTick(m1, m2, hold.steps[step].ease, xt, yt))
						return ID;
				}

				const Note& lastMid = score.notes.at(hold.steps[hold.steps.size() - 1].ID);
				if (isHoldPathInTick(lastMid, end, hold.steps[hold.steps.size() - 1].ease, xt, yt))
					return ID;
			}
			else
			{
				if (isHoldPathInTick(start, end, hold.start.ease, xt, yt))
					return ID;
			}
		}

		return -1;
	}

	void ScoreEditor::calcDragSelection()
	{
		float left = std::min(dragStart.x, mousePos.x);
		float right = std::max(dragStart.x, mousePos.x);
		float top = std::min(dragStart.y, mousePos.y);
		float bottom = std::max(dragStart.y, mousePos.y);

		if (!InputListener::isAltDown() && !InputListener::isCtrlDown())
			selection.clear();

		for (const auto& n : score.notes)
		{
			const Note& note = n.second;
			float x1 = canvas.laneToPosition(note.lane);
			float x2 = canvas.laneToPosition(note.lane + note.width);
			float y = -canvas.tickToPosition(note.tick);

			if (right > x1 && left < x2 && isWithinRange(y, top - 10.0f, bottom + 10.0f))
			{
				if (InputListener::isAltDown())
					selection.remove(note.ID);
				else
					selection.append(note.ID);
			}
		}
	}

	void ScoreEditor::updateNotes(Renderer* renderer)
	{
		// directxmath dies
		if (canvas.getSize().y < 10 || canvas.getSize().x < 10)
			return;

		Shader* shader = ResourceManager::shaders[0];
		shader->use();
		shader->setMatrix4("projection", camera.getOffCenterOrthographicProjection(0, canvas.getSize().x, canvas.getPosition().y, canvas.getPosition().y + canvas.getSize().y));

		ImDrawList* drawList = ImGui::GetWindowDrawList();

		framebuffer->bind();
		framebuffer->clear();
		renderer->beginBatch();

		for (auto& [id, note] : score.notes)
		{
			if (canvas.isNoteInCanvas(note.tick) && note.getType() == NoteType::Tap)
			{
				updateNote(note);
				drawNote(note, renderer, noteTint);
			}
		}

		for (auto& [id, hold] : score.holdNotes)
		{
			Note& start = score.notes.at(hold.start.ID);
			Note& end = score.notes.at(hold.end);

			if (canvas.isNoteInCanvas(start.tick)) updateNote(start);
			if (canvas.isNoteInCanvas(end.tick)) updateNote(end);

			for (const auto& step : hold.steps)
			{
				Note& mid = score.notes.at(step.ID);
				if (canvas.isNoteInCanvas(mid.tick)) updateNote(mid);

				if (skipUpdateAfterSortingSteps)
				{
					skipUpdateAfterSortingSteps = false;
					break;
				}
			}

			drawHoldNote(score.notes, hold, renderer, noteTint);
		}

		renderer->endBatch();
		renderer->beginBatch();

		if (isPasting() || insertingPreset && canvas.isMouseInCanvas())
			previewPaste(renderer);

		if (canvas.isMouseInCanvas() && !isHoldingNote && currentMode != TimelineMode::Select &&
			!isPasting() && !insertingPreset && !UI::isAnyPopupOpen())
		{
			updateDummyNotes();
			previewInput(renderer);

			if (ImGui::IsMouseClicked(0) && hoverTick >= 0 && !isHoveringNote)
			{
				executeInput();
			}

			if (insertingHold)
			{
				if (ImGui::IsMouseDown(0))
					updateDummyHold();
				else
				{
					insertHoldNote();
					insertingHold = false;
				}
			}
		}
		else
		{
			insertingHold = false;
		}

		renderer->endBatch();

		glDisable(GL_DEPTH_TEST);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		unsigned int fbTex = framebuffer->getTexture();
		drawList->AddImage((void*)fbTex, canvas.getPosition(), canvas.getPosition() + canvas.getSize());
		drawSelectionBoxes();
		drawStepOutlines();
		drawSteps.clear();
	}

	void ScoreEditor::previewInput(Renderer* renderer)
	{
		if (currentMode == TimelineMode::InsertLong)
		{
			drawDummyHold(renderer);
		}
		else if (currentMode == TimelineMode::InsertLongMid)
		{
			drawHoldMid(dummyMid, defaultStepType, renderer, hoverTint);
			drawOutline(StepDrawData{ dummyMid.tick, dummyMid.lane, dummyMid.width, defaultStepType });
		}
		else if (currentMode == TimelineMode::InsertBPM)
		{
			bpmControl(defaultBPM, hoverTick, false);
		}
		else if (currentMode == TimelineMode::InsertTimeSign)
		{
			timeSignatureControl(defaultTimeSignN, defaultTimeSignD, hoverTick, false);
		}
		else
		{
			drawNote(dummy, renderer, hoverTint);
		}
	}

	void ScoreEditor::executeInput()
	{
		if (currentMode == TimelineMode::InsertLong)
		{
			insertingHold = true;
		}
		else if (currentMode == TimelineMode::InsertLongMid)
		{
			int id = findClosestHold();
			if (id != -1)
				insertHoldStep(score.holdNotes.at(id));
		}
		else if (currentMode == TimelineMode::InsertBPM)
		{
			insertTempo();
		}
		else if (currentMode == TimelineMode::InsertTimeSign)
		{
			insertTimeSignature();
		}
		else
		{
			insertNote(currentMode == TimelineMode::MakeCritical);
		}
	}

	void ScoreEditor::updateDummyNotes()
	{
		dummy.lane = laneFromCenterPos(hoverLane, defaultNoteWidth);
		dummy.tick = hoverTick;
		dummy.width = defaultNoteWidth;
		dummyMid.lane = dummy.lane;
		dummyMid.tick = dummy.tick;
		dummyMid.width = dummy.width;
		if (!insertingHold)
		{
			dummyStart.lane = dummyEnd.lane = dummy.lane;
			dummyStart.width = dummyEnd.width = dummy.width;
			dummyStart.tick = dummyEnd.tick = dummy.tick;
		}
	}

	void ScoreEditor::updateDummyHold()
	{
		dummyEnd.lane = laneFromCenterPos(hoverLane, dummyEnd.width);
		dummyEnd.tick = hoverTick;
	}

	void ScoreEditor::changeMode(TimelineMode mode)
	{
		switch (mode)
		{
		case TimelineMode::InsertTap:
			dummy.flick = FlickType::None;
			dummy.critical = false;
			break;

		case TimelineMode::InsertFlick:
			dummy.flick = FlickType::Up;
			dummy.critical = false;
			break;

		case TimelineMode::MakeCritical:
			dummy.flick = FlickType::None;
			dummy.critical = true;
			break;

			// #C02: Save selected hold step type
		case TimelineMode::InsertLongMid:
			if (currentMode == TimelineMode::InsertLongMid)
				defaultStepType = (HoldStepType)(((int)defaultStepType + 1) % 3);
			break;

		default:
			break;
		}
		currentMode = mode;
	}

	void ScoreEditor::drawSelectionRectangle()
	{
		float startX = std::min(canvas.getPosition().x + dragStart.x, canvas.getPosition().x + mousePos.x);
		float endX = std::max(canvas.getPosition().x + dragStart.x, canvas.getPosition().x + mousePos.x);
		float startY = std::min(canvas.getPosition().y + dragStart.y, canvas.getPosition().y + mousePos.y) + canvas.getVisualOffset();
		float endY = std::max(canvas.getPosition().y + dragStart.y, canvas.getPosition().y + mousePos.y) + canvas.getVisualOffset();
		ImVec2 start{ startX, startY };
		ImVec2 end{ endX, endY };

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(start, end, selectionColor1);
		drawList->AddRect(start, end, 0xbbcccccc, 0.2f, ImDrawFlags_RoundCornersAll, 1.0f);

		ImVec2 iconPos = ImVec2(canvas.getPosition() + dragStart);
		iconPos.y += canvas.getVisualOffset();
		if (InputListener::isCtrlDown())
		{
			drawList->AddText(ImGui::GetFont(), 12, iconPos, 0xdddddddd, ICON_FA_PLUS_CIRCLE);
		}
		else if (InputListener::isAltDown())
		{
			drawList->AddText(ImGui::GetFont(), 12, iconPos, 0xdddddddd, ICON_FA_MINUS_CIRCLE);
		}
	}

	void ScoreEditor::drawSelectionBoxes()
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		if (!drawList)
			return;

		for (int id : selection.getSelection())
		{
			const Note& note = score.notes.at(id);
			if (!canvas.isNoteInCanvas(note.tick))
				continue;

			float x = canvas.getPosition().x;
			float y = canvas.getPosition().y - canvas.tickToPosition(note.tick) + canvas.getVisualOffset();
			
			ImVec2 p1 {
				x + canvas.laneToPosition(note.lane) - 1,
				y - (canvas.getNotesHeight() * 0.35f)
			};
			
			ImVec2 p2 {
				x + canvas.laneToPosition(note.lane + note.width) + 3,
				y + (canvas.getNotesHeight() * 0.35f)
			};

			drawList->AddRectFilled(p1, p2, 0x33555555, 2.0f, ImDrawFlags_RoundCornersAll);
			drawList->AddRect(p1, p2, 0xcccccccc, 2.0f, ImDrawFlags_RoundCornersAll, 1.5f);
		}
	}

	void ScoreEditor::drawStepOutlines()
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		if (!drawList)
			return;

		for (const auto& item : drawSteps)
			drawOutline(item);
	}

	bool ScoreEditor::isHoldPathInTick(const Note& n1, const Note& n2, EaseType ease, float x, float y)
	{
		float xStart1 = canvas.laneToPosition(n1.lane);
		float xStart2 = canvas.laneToPosition(n1.lane + n1.width);
		float xEnd1 = canvas.laneToPosition(n2.lane);
		float xEnd2 = canvas.laneToPosition(n2.lane + n2.width);
		float y1 = canvas.getNoteYPosFromTick(n1.tick);
		float y2 = canvas.getNoteYPosFromTick(n2.tick);

		if (y > y2 || y < y1)
			return false;

		float percent = (y - y1) / (y2 - y1);
		float iPercent = ease == EaseType::None ? percent : ease == EaseType::EaseIn ? easeIn(percent) : easeOut(percent);
		float xl = lerp(xStart1, xEnd1, iPercent);
		float xr = lerp(xStart2, xEnd2, iPercent);

		return isWithinRange(x, xl < xr ? xl : xr, xr > xl ? xr : xl);
	}

	std::string ScoreEditor::getWorkingFilename() const
	{
		return workingData.filename;
	}
}
