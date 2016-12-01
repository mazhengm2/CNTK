//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#define _CRT_SECURE_NO_WARNINGS // "secure" CRT not available on all platforms  --add this at the top of all CPP files that give "function or variable may be unsafe" warnings

#include "CNTKLibrary.h"
#include "Common.h"

using namespace CNTK;
using namespace std::placeholders;

extern bool Is1bitSGDAvailable();

namespace
{
    struct FeedForwardClassifier
    {
        size_t inputDim;
        size_t ouputDim;
        Variable features;
        Variable labels;
        FunctionPtr output;
        FunctionPtr trainingLoss;
        FunctionPtr prediction;
    };

    const std::wstring g_featureStreamName = L"features";
    const std::wstring g_labelsStreamName = L"labels";
    const std::wstring g_inputFile = L"SimpleDataTrain_cntk_text.txt";
    const size_t minibatchSize = 25;
    const size_t numSamplesPerSweep = 10000;
    const size_t numSweepsToTrainWith = 2;
    const size_t numMinibatchesToTrain = (numSamplesPerSweep * numSweepsToTrainWith) / minibatchSize;
    const size_t totalNumberOfSamples = numSamplesPerSweep * numSweepsToTrainWith;

    void LoopBasedOnSamples(const std::wstring& name, const DeviceDescriptor& device, std::function<DistributedLearnerPtr(const std::vector<LearnerPtr>&)> factory, const FeedForwardClassifier& classifier)
    {
        printf("Training loop thru samples with %ls.\n", name.c_str());

        auto minibatchSource = TextFormatMinibatchSource(g_inputFile,
            { { g_featureStreamName, classifier.inputDim }, { g_labelsStreamName, classifier.ouputDim } },
            totalNumberOfSamples,
            true,
            name == L"blockmomentum" ? MinibatchSource::InfiniteSamples: 0);

        auto featureStreamInfo = minibatchSource->StreamInfo(g_featureStreamName);
        auto labelStreamInfo = minibatchSource->StreamInfo(g_labelsStreamName);

        double learningRatePerSample = 0.02;

        Trainer trainer(classifier.output, classifier.trainingLoss, classifier.prediction, { factory({ SGDLearner(classifier.output->Parameters(), LearningRatePerSampleSchedule(learningRatePerSample)) }) });
        size_t outputFrequencyInMinibatches = 20;
        size_t currentCheckpointIndex = 0;
        size_t checkpointFrequency = 7000;
        size_t index = 0;
        bool updated = true;
        while (updated)
        {
            auto minibatchData = minibatchSource->GetNextMinibatch(minibatchSize, device);
            if (minibatchData.empty())
                updated = trainer.TrainMinibatch({}, device);
            else
                updated = trainer.TrainMinibatch({ { classifier.features, minibatchData[featureStreamInfo].m_data }, { classifier.labels, minibatchData[labelStreamInfo].m_data } }, device);

            size_t checkpointIndex = trainer.TotalNumberOfSamplesSeen() / checkpointFrequency;
            if (checkpointIndex > currentCheckpointIndex)
            {
                trainer.SaveCheckpoint(L"test");
                currentCheckpointIndex = checkpointIndex;
            }

            PrintTrainingProgress(trainer, index++, outputFrequencyInMinibatches);
        }
    }

    FeedForwardClassifier BuildFeedForwardClassifer(const DeviceDescriptor& device)
    {
        const size_t inputDim = 2;
        const size_t numOutputClasses = 2;
        const size_t hiddenLayerDim = 50;
        const size_t numHiddenLayers = 2;

        auto minibatchSource = TextFormatMinibatchSource(g_inputFile, { { g_featureStreamName, inputDim }, { g_labelsStreamName, numOutputClasses } }, MinibatchSource::FullDataSweep, false);
        auto featureStreamInfo = minibatchSource->StreamInfo(g_featureStreamName);
        auto labelStreamInfo = minibatchSource->StreamInfo(g_labelsStreamName);

        std::unordered_map<StreamInformation, std::pair<NDArrayViewPtr, NDArrayViewPtr>> inputMeansAndInvStdDevs = { { featureStreamInfo, { nullptr, nullptr } } };
        ComputeInputPerDimMeansAndInvStdDevs(minibatchSource, inputMeansAndInvStdDevs);

        auto nonLinearity = std::bind(Sigmoid, _1, L"Sigmoid");
        auto input = InputVariable({ inputDim }, DataType::Float, g_featureStreamName);
        auto normalizedinput = PerDimMeanVarianceNormalize(input, inputMeansAndInvStdDevs[featureStreamInfo].first, inputMeansAndInvStdDevs[featureStreamInfo].second);
        auto classifierOutput = FullyConnectedDNNLayer(normalizedinput, hiddenLayerDim, device, nonLinearity, std::wstring(L"FullyConnectedInput"));
        for (size_t i = 1; i < numHiddenLayers; ++i)
            classifierOutput = FullyConnectedDNNLayer(classifierOutput, hiddenLayerDim, device, nonLinearity, std::wstring(L"FullyConnectedHidden"));

        auto outputTimesParam = Parameter(NDArrayView::RandomUniform<float>({ numOutputClasses, hiddenLayerDim }, -0.05, 0.05, 1, device), L"outputTimesParam");
        auto outputBiasParam = Parameter(NDArrayView::RandomUniform<float>({ numOutputClasses }, -0.05, 0.05, 1, device), L"outputBiasParam");
        classifierOutput = Plus(outputBiasParam, Times(outputTimesParam, classifierOutput), L"classifierOutput");

        auto labels = InputVariable({ numOutputClasses }, DataType::Float, g_labelsStreamName);
        auto trainingLoss = CNTK::CrossEntropyWithSoftmax(classifierOutput, labels, L"lossFunction");
        auto prediction = CNTK::ClassificationError(classifierOutput, labels, L"classificationError");

        return FeedForwardClassifier{ inputDim, numOutputClasses, input, labels, classifierOutput, trainingLoss, prediction };
    }
}

void TestFrameMode()
{
    std::this_thread::sleep_for(std::chrono::seconds(16));
    // Create a set of trainers.
    std::map<std::wstring, std::function<DistributedLearnerPtr(const std::vector<LearnerPtr>&)>> learners;
    learners[L"simple"] = [](const std::vector<LearnerPtr>& l) { return CreateDataParallelDistributedLearner(MPICommunicator(), l, 0); };

    if (Is1bitSGDAvailable())
    {
        learners[L"1bitsgd"] = [](const std::vector<LearnerPtr>& l) { return CreateQuantizedDataParallelDistributedLearner(QuantizedMPICommunicator(true, true, 32), l, 0); };
        learners[L"blockmomentum"] = [](const std::vector<LearnerPtr>& l) { return CreateBlockMomentumDistributedLearner(MPICommunicator(), l, 0, 1024); };
    }

    // Create a set of devices.
    std::vector<DeviceDescriptor> devices;
    devices.push_back(DeviceDescriptor::CPUDevice());
    if (IsGPUAvailable())
        devices.push_back(DeviceDescriptor::GPUDevice(0));

    // Create different types of loops.
    std::vector<std::function<void(const std::wstring&, const DeviceDescriptor&, std::function<DistributedLearnerPtr(const std::vector<LearnerPtr>&)>, const FeedForwardClassifier&)>> loops;
    loops.push_back(LoopBasedOnSamples);

    // Trying all distribution methods on all available devices with different types of loops.
    auto sync = MPICommunicator();
    for (auto l : learners)
    {
        for (auto device : devices)
        {
            for (auto loop : loops)
            {
                sync->Barrier();
                loop(l.first, device, l.second, BuildFeedForwardClassifer(device));
            }
        }
    }
    sync->Barrier();
}